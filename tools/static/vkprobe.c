/* vkprobe.c —— 原生 Vulkan 每批成本探针（不需要着色器）
 *
 * 用 vkCmdCopyBuffer 制造真实 GPU 工作，每帧记录 N 次提交（batch boundary），
 * 每次提交里 D 个 copy。从而把「每次提交的成本」和「每个命令的成本」分开。
 *
 * 用法：
 *   ./vkprobe -n 400 -d 1 -s      每帧 400 次提交，每次 1 个 copy，每次提交后等 fence
 *   ./vkprobe -n 400 -d 1         每帧 400 次提交，只在帧末统一等
 *   ./vkprobe -n 1 -d 400         每帧 1 次提交，里面 400 个 copy
 *
 * 判据：
 *   -s 与不带 -s 差很多  ⇒ 成本在「等 fence」上（同步粒度）
 *   两者差不多            ⇒ 成本在「提交」本身
 *   -n 与 -d 都线性增长   ⇒ 每个命令都有固定成本（更像延迟而非计算）
 *
 * 环境变量：
 *   VKPROBE_VID / VKPROBE_DID   指定厂商/设备 ID（默认 0x10de / 0x1b87 = P104-100）
 *   VK_ICD_FILENAMES            用哪个 ICD
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s 失败: %d (line %d)\n", #x, _r, __LINE__); exit(4);} } while (0)

static uint32_t pick_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

int main(int argc, char **argv) {
    int nSub = 400, nCopy = 1, nFrame = 60, nWarm = 10, waitEach = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) nSub = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) nCopy = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) nFrame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) nWarm = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) waitEach = 1;
    }
    if (nSub < 1 || nSub > 4000) { fprintf(stderr, "-n 范围 1..4000\n"); return 2; }

    uint32_t vid = 0x10de, did = 0x1b87;
    { const char *e = getenv("VKPROBE_VID"); if (e) vid = (uint32_t)strtoul(e, NULL, 0); }
    { const char *e = getenv("VKPROBE_DID"); if (e) did = (uint32_t)strtoul(e, NULL, 0); }

    /* ---- instance ---- */
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vkprobe"; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));

    /* ---- 选设备 ---- */
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) { fprintf(stderr, "没有 Vulkan 设备\n"); return 2; }
    VkPhysicalDevice *pds = malloc(nd * sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &nd, pds);

    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties prop;
    for (uint32_t i = 0; i < nd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &prop);
        if (prop.vendorID == vid && prop.deviceID == did) { pd = pds[i]; break; }
    }
    if (!pd) {
        fprintf(stderr, "找不到 %04x:%04x，现有设备：\n", vid, did);
        for (uint32_t i = 0; i < nd; i++) {
            vkGetPhysicalDeviceProperties(pds[i], &prop);
            fprintf(stderr, "  %04x:%04x  %s\n", prop.vendorID, prop.deviceID, prop.deviceName);
        }
        return 2;
    }
    printf("device       : %s\n", prop.deviceName);
    printf("apiVersion   : %u.%u.%u\n", VK_API_VERSION_MAJOR(prop.apiVersion),
           VK_API_VERSION_MINOR(prop.apiVersion), VK_API_VERSION_PATCH(prop.apiVersion));

    /* ---- queue ---- */
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qf = malloc(nq * sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) { qi = i; break; }
    if (qi == UINT32_MAX) { fprintf(stderr, "无可用队列\n"); return 2; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    /* ---- 两个 buffer（copy 用，无需着色器）---- */
    const VkDeviceSize SZ = 4096;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = SZ;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBuffer buf[2]; VkDeviceMemory mem[2];
    for (int k = 0; k < 2; k++) {
        CK(vkCreateBuffer(dev, &bci, NULL, &buf[k]));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf[k], &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = pick_mem(pd, mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CK(vkAllocateMemory(dev, &mai, NULL, &mem[k]));
        CK(vkBindBufferMemory(dev, buf[k], mem[k], 0));
    }

    /* ---- command pool + N 个 cmdbuf + N 个 fence ---- */
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qi;
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    VkCommandBuffer *cb = malloc(nSub * sizeof(*cb));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = nSub;
    CK(vkAllocateCommandBuffers(dev, &cai, cb));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence *fe = malloc(nSub * sizeof(*fe));
    for (int i = 0; i < nSub; i++) CK(vkCreateFence(dev, &fci, NULL, &fe[i]));

    VkBufferCopy reg = { 0, 0, SZ };

    int total = nWarm + nFrame;
    double *ms = malloc(sizeof(double) * total);

    for (int f = 0; f < total; f++) {
        double a = now_ms();
        for (int i = 0; i < nSub; i++) {
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            CK(vkBeginCommandBuffer(cb[i], &bi));
            for (int c = 0; c < nCopy; c++)
                vkCmdCopyBuffer(cb[i], buf[0], buf[1], 1, &reg);
            CK(vkEndCommandBuffer(cb[i]));

            CK(vkResetFences(dev, 1, &fe[i]));
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &cb[i];
            CK(vkQueueSubmit(q, 1, &si, fe[i]));

            if (waitEach) CK(vkWaitForFences(dev, 1, &fe[i], VK_TRUE, UINT64_MAX));
        }
        if (!waitEach) CK(vkWaitForFences(dev, nSub, fe, VK_TRUE, UINT64_MAX));
        ms[f] = now_ms() - a;
    }

    /* 只统计后 nFrame 个（跳过 warmup） */
    double *real = ms + nWarm;
    qsort(real, nFrame, sizeof(double), cmpd);
    double p50 = real[nFrame / 2];
    double p95 = real[(int)(nFrame * 0.95)];
    double perSub = p50 * 1000.0 / nSub;
    double perCmd = p50 * 1000.0 / ((double)nSub * nCopy);

    printf("wait_each    : %d\n", waitEach);
    printf("submits/frame: %d\n", nSub);
    printf("copies/submit: %d\n", nCopy);
    printf("total_p50    : %.3f ms\n", p50);
    printf("total_p95    : %.3f ms\n", p95);
    printf("per_submit   : %.1f us\n", perSub);
    printf("per_cmd      : %.1f us\n", perCmd);
    printf("fps          : %.2f\n", 1000.0 / p50);

    for (int i = 0; i < nSub; i++) vkDestroyFence(dev, fe[i], NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    for (int k = 0; k < 2; k++) { vkDestroyBuffer(dev, buf[k], NULL); vkFreeMemory(dev, mem[k], NULL); }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
