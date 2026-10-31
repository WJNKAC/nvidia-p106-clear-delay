/* vkdraw.c —— 原生 Vulkan **3D 绘制**每批成本探针
 *
 * 与 vkprobe.c（只做 copy）的区别：这里跑真正的图形管线 + vkCmdDraw，
 * 覆盖的是 3D 路径 —— 也就是受限 SKU 那条延迟所属的路径。
 *
 * 用法：
 *   ./vkdraw -n 400 -d 1 -s    每帧 400 次提交，每次 1 个 draw，每次提交后等 fence
 *   ./vkdraw -n 1 -d 400       每帧 1 次提交，里面 400 个 draw
 *
 * 判据：
 *   per_submit 随 -n 线性增长且量级达 ~100µs  ⇒ 每批固定延迟存在
 *   per_cmd 极小（<1µs）                       ⇒ 每个 draw 本身很便宜
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
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

/* 读 SPIR-V 文件 */
static VkShaderModule load_spv(VkDevice dev, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); exit(3); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "读 %s 失败\n", path); exit(3); }
    fclose(f);
    VkShaderModuleCreateInfo sci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    sci.codeSize = sz; sci.pCode = (const uint32_t *)buf;
    VkShaderModule m; CK(vkCreateShaderModule(dev, &sci, NULL, &m));
    free(buf);
    return m;
}

int main(int argc, char **argv) {
    const char *vspv = "/tmp/tri.vert.spv", *fspv = "/tmp/tri.frag.spv";
    int nSub = 400, nDraw = 1, nFrame = 60, nWarm = 10, waitEach = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) nSub = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) nDraw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) nFrame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) nWarm = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) waitEach = 1;
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) vspv = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) fspv = argv[++i];
    }
    if (nSub < 1 || nSub > 4000) { fprintf(stderr, "-n 范围 1..4000\n"); return 2; }

    uint32_t vid = 0x10de, did = 0x1b87;
    { const char *e = getenv("VKPROBE_VID"); if (e) vid = (uint32_t)strtoul(e, NULL, 0); }
    { const char *e = getenv("VKPROBE_DID"); if (e) did = (uint32_t)strtoul(e, NULL, 0); }

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vkdraw"; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    VkPhysicalDevice *pds = malloc(nd * sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &nd, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE; VkPhysicalDeviceProperties prop;
    for (uint32_t i = 0; i < nd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &prop);
        if (prop.vendorID == vid && prop.deviceID == did) { pd = pds[i]; break; }
    }
    if (!pd) { fprintf(stderr, "找不到 %04x:%04x\n", vid, did); return 2; }
    printf("device       : %s\n", prop.deviceName);

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qf = malloc(nq * sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
    if (qi == UINT32_MAX) { fprintf(stderr, "无图形队列\n"); return 2; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    /* ---- 离屏颜色附件 ---- */
    const uint32_t W = 256, H = 256;
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = fmt;
    ic.extent.width = W; ic.extent.height = H; ic.extent.depth = 1;
    ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; CK(vkCreateImage(dev, &ic, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = pick_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &mai, NULL, &imem));
    CK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = img; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = fmt;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView iv; CK(vkCreateImageView(dev, &ivci, NULL, &iv));

    /* ---- render pass ---- */
    VkAttachmentDescription att = {0};
    att.format = fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {0};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0; dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount = 1; rpci.pSubpasses = &sp;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

    VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbci.renderPass = rp; fbci.attachmentCount = 1; fbci.pAttachments = &iv;
    fbci.width = W; fbci.height = H; fbci.layers = 1;
    VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

    /* ---- pipeline ---- */
    VkShaderModule vs = load_spv(dev, vspv), fs = load_spv(dev, fspv);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount = 2; gpci.pStages = st;
    gpci.pVertexInputState = &vi; gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms; gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &ds; gpci.layout = pl; gpci.renderPass = rp; gpci.subpass = 0;
    VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    /* ---- pool / cmdbuf / fence ---- */
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qi;
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBuffer *cmd = malloc(nSub * sizeof(*cmd));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = nSub;
    CK(vkAllocateCommandBuffers(dev, &cai, cmd));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence *fe = malloc(nSub * sizeof(*fe));
    for (int i = 0; i < nSub; i++) CK(vkCreateFence(dev, &fci, NULL, &fe[i]));

    VkViewport vport = { 0, 0, (float)W, (float)H, 0, 1 };
    VkRect2D scis = { {0,0}, {W,H} };
    VkClearValue cv; memset(&cv, 0, sizeof cv); cv.color.float32[3] = 1.0f;

    int total = nWarm + nFrame;
    double *ms = malloc(sizeof(double) * total);

    for (int f = 0; f < total; f++) {
        double a = now_ms();
        for (int i = 0; i < nSub; i++) {
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            CK(vkBeginCommandBuffer(cmd[i], &bi));
            VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rbi.renderPass = rp; rbi.framebuffer = fb;
            rbi.renderArea = (VkRect2D){ {0,0}, {W,H} };
            rbi.clearValueCount = 1; rbi.pClearValues = &cv;
            vkCmdBeginRenderPass(cmd[i], &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdSetViewport(cmd[i], 0, 1, &vport);
            vkCmdSetScissor(cmd[i], 0, 1, &scis);
            for (int d = 0; d < nDraw; d++) vkCmdDraw(cmd[i], 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd[i]);
            CK(vkEndCommandBuffer(cmd[i]));

            CK(vkResetFences(dev, 1, &fe[i]));
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &cmd[i];
            CK(vkQueueSubmit(q, 1, &si, fe[i]));
            if (waitEach) CK(vkWaitForFences(dev, 1, &fe[i], VK_TRUE, UINT64_MAX));
        }
        if (!waitEach) CK(vkWaitForFences(dev, nSub, fe, VK_TRUE, UINT64_MAX));
        ms[f] = now_ms() - a;
    }

    double *real = ms + nWarm;
    qsort(real, nFrame, sizeof(double), cmpd);
    double p50 = real[nFrame / 2];
    double p95 = real[(int)(nFrame * 0.95)];
    printf("wait_each    : %d\n", waitEach);
    printf("submits/frame: %d\n", nSub);
    printf("draws/submit : %d\n", nDraw);
    printf("total_p50    : %.3f ms\n", p50);
    printf("total_p95    : %.3f ms\n", p95);
    printf("per_submit   : %.1f us\n", p50 * 1000.0 / nSub);
    printf("per_draw     : %.1f us\n", p50 * 1000.0 / ((double)nSub * nDraw));
    printf("fps          : %.2f\n", 1000.0 / p50);

    for (int i = 0; i < nSub; i++) vkDestroyFence(dev, fe[i], NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);
    vkDestroyFramebuffer(dev, fb, NULL); vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyImageView(dev, iv, NULL); vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, imem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    return 0;
}
