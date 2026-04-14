extern unsigned char *WriteBuf;
extern unsigned char *DisplayBuf;
extern unsigned char *FrameBuf;
extern unsigned char *LayerBuf;

static uint32_t *host_fb_framebuffer = NULL;
static uint32_t *host_fb_layerbuffer = NULL;
static uint32_t host_fb_layer_transparent = 0;
static uint64_t host_fb_next_merge_us = 0;
static uint32_t *host_fb_copy_src = NULL;
static uint32_t *host_fb_copy_dst = NULL;
static int host_fb_copy_pending = 0;

static void host_fb_bind_display(void) {
    if (!host_framebuffer) return;
    DisplayBuf = (unsigned char *)host_framebuffer;
    if (FrameBuf == NULL) FrameBuf = DisplayBuf;
    if (LayerBuf == NULL) LayerBuf = DisplayBuf;
}

static uint32_t *host_fb_buffer_for_target(unsigned char *target) {
    if (!host_framebuffer) host_fb_ensure();
    if (!host_framebuffer) return NULL;
    host_fb_bind_display();
    if (target == NULL || target == DisplayBuf) return host_framebuffer;
    if (target == FrameBuf && host_fb_framebuffer) return host_fb_framebuffer;
    if (target == LayerBuf && host_fb_layerbuffer) return host_fb_layerbuffer;
    return host_framebuffer;
}

static uint32_t *host_fb_current_target(void) {
    return host_fb_buffer_for_target(WriteBuf);
}

static void host_fb_fill_buffer(uint32_t *buffer, uint32_t colour) {
    size_t pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    if (!buffer) return;
    for (size_t i = 0; i < pixels; ++i) buffer[i] = colour;
}

static void host_fb_merge_now(uint32_t transparent) {
    size_t pixels;
    if (!host_framebuffer) host_fb_ensure();
    if (!host_framebuffer || !host_fb_framebuffer || !host_fb_layerbuffer) return;
    pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t layer = host_fb_layerbuffer[i];
        host_framebuffer[i] = (layer == transparent) ? host_fb_framebuffer[i] : layer;
    }
}

static void host_fb_copy_now(uint32_t *src, uint32_t *dst) {
    size_t pixels;
    if (!src || !dst) return;
    pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    memcpy(dst, src, pixels * sizeof(*dst));
}

static void host_fb_complete_pending_copy(void) {
    if (!host_fb_copy_pending) return;
    host_fb_copy_now(host_fb_copy_src, host_fb_copy_dst);
    host_fb_copy_src = NULL;
    host_fb_copy_dst = NULL;
    host_fb_copy_pending = 0;
}

void host_framebuffer_reset_runtime(int colour) {
    if (!host_framebuffer) host_fb_ensure();
    if (host_fb_framebuffer) free(host_fb_framebuffer);
    if (host_fb_layerbuffer) free(host_fb_layerbuffer);
    host_fb_framebuffer = NULL;
    host_fb_layerbuffer = NULL;
    host_fb_layer_transparent = 0;
    mergerunning = 0;
    mergetimer = 0;
    host_fb_next_merge_us = 0;
    host_fb_copy_src = NULL;
    host_fb_copy_dst = NULL;
    host_fb_copy_pending = 0;
    host_fb_bind_display();
    FrameBuf = DisplayBuf;
    LayerBuf = DisplayBuf;
    WriteBuf = NULL;
    host_fb_fill_buffer(host_framebuffer, host_colour24(colour));
}

void host_framebuffer_shutdown_runtime(void) {
    mergerunning = 0;
    mergetimer = 0;
    host_fb_next_merge_us = 0;
    host_fb_copy_src = NULL;
    host_fb_copy_dst = NULL;
    host_fb_copy_pending = 0;
    if (host_fb_framebuffer) free(host_fb_framebuffer);
    if (host_fb_layerbuffer) free(host_fb_layerbuffer);
    host_fb_framebuffer = NULL;
    host_fb_layerbuffer = NULL;
    host_fb_layer_transparent = 0;
    host_fb_bind_display();
    if (DisplayBuf != NULL) {
        FrameBuf = DisplayBuf;
        LayerBuf = DisplayBuf;
    }
    WriteBuf = NULL;
}

void host_framebuffer_clear_target(int colour) {
    uint32_t *target = host_fb_current_target();
    host_fb_fill_buffer(target, host_colour24(colour));
}

void host_framebuffer_create(void) {
    size_t pixels;
    if (!host_framebuffer) host_fb_ensure();
    host_fb_bind_display();
    if (FrameBuf != NULL && FrameBuf != DisplayBuf) error("Framebuffer already exists");
    pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    host_fb_framebuffer = calloc(pixels, sizeof(*host_fb_framebuffer));
    if (!host_fb_framebuffer) error("Not enough memory");
    FrameBuf = (unsigned char *)host_fb_framebuffer;
}

void host_framebuffer_layer(int has_colour, int colour) {
    size_t pixels;
    uint32_t fill = has_colour ? host_colour24(colour) : 0;
    if (!host_framebuffer) host_fb_ensure();
    host_fb_bind_display();
    if (LayerBuf != NULL && LayerBuf != DisplayBuf) error("Framebuffer already exists");
    pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    host_fb_layerbuffer = calloc(pixels, sizeof(*host_fb_layerbuffer));
    if (!host_fb_layerbuffer) error("Not enough memory");
    host_fb_layer_transparent = fill;
    host_fb_fill_buffer(host_fb_layerbuffer, fill);
    LayerBuf = (unsigned char *)host_fb_layerbuffer;
}

void host_framebuffer_write(char which) {
    host_fb_bind_display();
    switch ((char)toupper((unsigned char)which)) {
        case 'N':
            if (mergerunning) error("Display in use for merged operation");
            WriteBuf = NULL;
            return;
        case 'F':
            if (FrameBuf == NULL || FrameBuf == DisplayBuf) error("Frame buffer not created");
            WriteBuf = FrameBuf;
            return;
        case 'L':
            if (LayerBuf == NULL || LayerBuf == DisplayBuf) error("Layer buffer not created");
            WriteBuf = LayerBuf;
            return;
        default:
            error("Syntax");
    }
}

void host_framebuffer_close(char which) {
    host_fb_bind_display();
    if (which == 0 || which == BC_FB_TARGET_DEFAULT) which = 'A';
    which = (char)toupper((unsigned char)which);
    mergerunning = 0;
    mergetimer = 0;
    host_fb_next_merge_us = 0;
    host_fb_copy_src = NULL;
    host_fb_copy_dst = NULL;
    host_fb_copy_pending = 0;
    if (which == 'A' || which == 'F') {
        if (WriteBuf == FrameBuf) WriteBuf = NULL;
        if (host_fb_framebuffer) free(host_fb_framebuffer);
        host_fb_framebuffer = NULL;
        FrameBuf = DisplayBuf;
    }
    if (which == 'A' || which == 'L') {
        if (WriteBuf == LayerBuf) WriteBuf = NULL;
        if (host_fb_layerbuffer) free(host_fb_layerbuffer);
        host_fb_layerbuffer = NULL;
        LayerBuf = DisplayBuf;
    }
    if (which != 'A' && which != 'F' && which != 'L') error("Syntax");
}

void host_framebuffer_merge(int has_colour, int colour, int mode, int has_rate, int rate_ms) {
    uint32_t transparent = has_colour ? host_colour24(colour) : 0;
    if (LayerBuf == NULL || LayerBuf == DisplayBuf) error("Layer not created");
    if (FrameBuf == NULL || FrameBuf == DisplayBuf) error("Framebuffer not created");
    if (has_rate && rate_ms < 0) error("Number out of bounds");
    switch (mode) {
        case BC_FB_MERGE_MODE_NOW:
        case BC_FB_MERGE_MODE_B:
            mergerunning = 0;
            mergetimer = 0;
            host_fb_next_merge_us = 0;
            host_fb_merge_now(transparent);
            return;
        case BC_FB_MERGE_MODE_R:
            host_fb_layer_transparent = transparent;
            mergerunning = 1;
            mergetimer = (uint32_t)(has_rate ? rate_ms : 0);
            if (WriteBuf == NULL || WriteBuf == DisplayBuf) WriteBuf = FrameBuf;
            host_fb_merge_now(transparent);
            host_fb_next_merge_us = host_now_us() + (uint64_t)mergetimer * 1000ULL;
            return;
        case BC_FB_MERGE_MODE_A:
            mergerunning = 0;
            mergetimer = 0;
            host_fb_next_merge_us = 0;
            return;
        default:
            error("Syntax");
    }
}

void host_framebuffer_sync(void) {
    host_fb_complete_pending_copy();
    if (!mergerunning) return;
    host_fb_merge_now(host_fb_layer_transparent);
    host_fb_next_merge_us = host_now_us() + (uint64_t)mergetimer * 1000ULL;
}

void host_framebuffer_wait(void) {
    host_fb_complete_pending_copy();
}

void host_framebuffer_copy(char from, char to, int background) {
    uint32_t *src = NULL;
    uint32_t *dst = NULL;

    host_fb_bind_display();
    from = (char)toupper((unsigned char)from);
    to = (char)toupper((unsigned char)to);

    if (from == 'N') src = host_framebuffer;
    else if (from == 'F') {
        if (FrameBuf == NULL || FrameBuf == DisplayBuf) error("Frame buffer not created");
        src = host_fb_framebuffer;
    } else if (from == 'L') {
        if (LayerBuf == NULL || LayerBuf == DisplayBuf) error("Layer buffer not created");
        src = host_fb_layerbuffer;
    } else error("Syntax");

    if (to == 'N') dst = host_framebuffer;
    else if (to == 'F') {
        if (FrameBuf == NULL || FrameBuf == DisplayBuf) error("Frame buffer not created");
        dst = host_fb_framebuffer;
    } else if (to == 'L') {
        if (LayerBuf == NULL || LayerBuf == DisplayBuf) error("Layer buffer not created");
        dst = host_fb_layerbuffer;
    } else error("Syntax");

    if (src == dst) return;
    if (background && dst == host_framebuffer) {
        host_fb_copy_src = src;
        host_fb_copy_dst = dst;
        host_fb_copy_pending = 1;
        return;
    }
    host_fb_copy_now(src, dst);
}

void host_framebuffer_service(void) {
    uint64_t now;
    host_fb_complete_pending_copy();
    if (!mergerunning) return;
    now = host_now_us();
    if (mergetimer != 0 && now < host_fb_next_merge_us) return;
    host_fb_merge_now(host_fb_layer_transparent);
    if (mergetimer != 0) host_fb_next_merge_us = now + (uint64_t)mergetimer * 1000ULL;
}
