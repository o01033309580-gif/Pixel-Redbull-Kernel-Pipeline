#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <linux/fs.h>
#include <linux/dma-buf.h>
#include <linux/stat.h>

#define MAX_HOOKS 16
#define MAX_FRAMES 64

static struct video_device* g_target_vdevs[MAX_HOOKS];
static const struct v4l2_ioctl_ops* g_orig_ops[MAX_HOOKS];
static struct v4l2_ioctl_ops* g_hook_ops[MAX_HOOKS];
static int g_hook_count = 0;

static void* g_frames[MAX_FRAMES];
static size_t g_frame_size = 1789952; // Default for 0x7FA30C09 1024x768 stride 1536
static int g_num_frames = 0;
static atomic_t g_frame_idx = ATOMIC_INIT(0);

static int inject_enable = 0;
module_param(inject_enable, int, 0644);

static atomic_t s_fmt_cnt = ATOMIC_INIT(0);
static atomic_t dqbuf_cnt = ATOMIC_INIT(0);

static int load_frames(const char* path) {
    struct file* filp;
    loff_t pos = 0;
    ssize_t bytes;
    int i;

    filp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(filp)) {
        pr_err("camhook: cannot open %s\n", path);
        return PTR_ERR(filp);
    }

    g_num_frames = 1; // Assuming 1 frame for now since we have real_1.bin
    pr_info("camhook: loading frames from %s\n", path);

    for (i = 0; i < g_num_frames; i++) {
        g_frames[i] = kmalloc(g_frame_size, GFP_KERNEL);
        if (!g_frames[i]) { 
            pr_err("camhook: kmalloc failed for frame %d\n", i); 
            break; 
        }
        bytes = kernel_read(filp, g_frames[i], g_frame_size, &pos);
        if (bytes != g_frame_size) {
            pr_err("camhook: short read frame %d: %zd\n", i, bytes);
            kfree(g_frames[i]);
            g_frames[i] = NULL;
            g_num_frames = i;
            break;
        }
    }
    filp_close(filp, NULL);
    pr_info("camhook: loaded %d frames\n", g_num_frames);
    return g_num_frames > 0 ? 0 : -ENOMEM;
}

static int inject_into_buffer(int dma_fd, void* frame_data, size_t frame_size) {
    struct dma_buf* dmabuf;
    void* vaddr;

    dmabuf = dma_buf_get(dma_fd);
    if (IS_ERR(dmabuf)) return PTR_ERR(dmabuf);

    vaddr = dma_buf_vmap(dmabuf);
    if (!vaddr) {
        dma_buf_put(dmabuf);
        return -ENOMEM;
    }

    dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
    memcpy(vaddr, frame_data, frame_size);
    dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);

    dma_buf_vunmap(dmabuf, vaddr);
    dma_buf_put(dmabuf);
    return 0;
}

static int hook_s_fmt_vid_cap(struct file* f, void* fh, struct v4l2_format* fmt) {
    struct video_device *vdev = video_devdata(f);
    int i;
    int (*orig_s_fmt)(struct file*, void*, struct v4l2_format*) = NULL;
    int ret, n;

    for (i = 0; i < g_hook_count; i++) {
        if (g_target_vdevs[i] == vdev) {
            orig_s_fmt = g_orig_ops[i]->vidioc_s_fmt_vid_cap;
            break;
        }
    }
    
    if (!orig_s_fmt) return -EINVAL;

    ret = orig_s_fmt(f, fh, fmt);
    n = atomic_inc_return(&s_fmt_cnt);
    if (n <= 10) {
        pr_info("camhook S_FMT[%d]: vdev=%s pixelformat=0x%08x w=%u h=%u bytesperline=%u sizeimage=%u\n",
                n, vdev->name, fmt->fmt.pix.pixelformat,
                fmt->fmt.pix.width, fmt->fmt.pix.height,
                fmt->fmt.pix.bytesperline, fmt->fmt.pix.sizeimage);
    }
    return ret;
}

static int hook_dqbuf(struct file* f, void* fh, struct v4l2_buffer* buf) {
    struct video_device *vdev = video_devdata(f);
    int i;
    int (*orig_dqbuf)(struct file*, void*, struct v4l2_buffer*) = NULL;
    int ret, n, idx;

    for (i = 0; i < g_hook_count; i++) {
        if (g_target_vdevs[i] == vdev) {
            orig_dqbuf = g_orig_ops[i]->vidioc_dqbuf;
            break;
        }
    }
    
    if (!orig_dqbuf) return -EINVAL;

    ret = orig_dqbuf(f, fh, buf);
    if (ret < 0) return ret;

    n = atomic_inc_return(&dqbuf_cnt);
    if (n <= 5) {
        pr_info("camhook DQBUF[%d]: idx=%u memory=%u bytesused=%u m.fd=%d\n",
                n, buf->index, buf->memory, buf->bytesused, buf->m.fd);
    }

    if (inject_enable && g_num_frames > 0 && buf->memory == V4L2_MEMORY_DMABUF) {
        idx = atomic_fetch_add(1, &g_frame_idx) % g_num_frames;
        inject_into_buffer(buf->m.fd, g_frames[idx], g_frame_size);
    }

    return ret;
}

static int __init camhook_init(void) {
    int i;
    struct video_device *vdev;
    char path[32];

    pr_info("camhook: init starting...\n");

    load_frames("/data/vendor/camera/real_1.bin"); // Try vendor path for SELinux context
    if (g_num_frames == 0) {
        load_frames("/data/local/tmp/real_1.bin"); // Fallback
    }

    // Iterate all minors
    for (i = 0; i < 64 && g_hook_count < MAX_HOOKS; i++) {
        struct file *f;
        
        snprintf(path, sizeof(path), "/dev/video%d", i);
        // Try to open the file
        f = filp_open(path, O_RDONLY | O_NONBLOCK, 0);
        if (IS_ERR(f)) {
            continue;
        }

        vdev = video_devdata(f);
        if (vdev && vdev->ioctl_ops) {
            if (strstr(vdev->name, "vicodec") || strstr(vdev->name, "vim2m")) {
                filp_close(f, NULL);
                continue;
            }

            g_target_vdevs[g_hook_count] = vdev;
            g_orig_ops[g_hook_count] = vdev->ioctl_ops;
            
            g_hook_ops[g_hook_count] = kmemdup(vdev->ioctl_ops, sizeof(struct v4l2_ioctl_ops), GFP_KERNEL);
            if (g_hook_ops[g_hook_count]) {
                if (g_hook_ops[g_hook_count]->vidioc_s_fmt_vid_cap)
                    g_hook_ops[g_hook_count]->vidioc_s_fmt_vid_cap = hook_s_fmt_vid_cap;
                if (g_hook_ops[g_hook_count]->vidioc_dqbuf)
                    g_hook_ops[g_hook_count]->vidioc_dqbuf = hook_dqbuf;
                
                vdev->ioctl_ops = g_hook_ops[g_hook_count];
                pr_info("camhook: hooked vdev minor=%d name=%s\n", i, vdev->name);
                g_hook_count++;
            }
        }
        filp_close(f, NULL);
    }

    pr_info("camhook: hooked %d video devices\n", g_hook_count);
    return 0;
}

static void __exit camhook_exit(void) {
    int i;
    for (i = 0; i < g_hook_count; i++) {
        g_target_vdevs[i]->ioctl_ops = g_orig_ops[i];
        kfree(g_hook_ops[i]);
    }
    
    for (i = 0; i < g_num_frames; i++) {
        if (g_frames[i]) kfree(g_frames[i]);
    }
    pr_info("camhook: exit complete\n");
}

module_init(camhook_init);
module_exit(camhook_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("camshim");
MODULE_DESCRIPTION("V4L2 camera ioctl hook for Pixel");
