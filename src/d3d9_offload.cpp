// Direct3D 9 part: the device is created as Direct3D 9Ex and every D3DPOOL_MANAGED resource is
// created in D3DPOOL_DEFAULT instead. The managed pool keeps a system-memory copy of each resource
// inside the game's 32-bit address space; DEFAULT resources live in video memory, and Windows
// (WDDM) pages them to system RAM outside the process when video memory is full. So texture and
// mesh data no longer count against the game's 4 GB.
//
// Locks on offloaded textures are emulated: the game writes into a SYSTEMMEM upload texture,
// UnlockRect copies it into the video-memory texture with UpdateSurface.
#include "common.h"
#include <d3d9.h>
#include <deque>
#include <vector>
#include <atomic>
#include <intrin.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace memx {

// ---------------------------------------------------------------- fast pointer hash
static __forceinline uint32_t ptr_hash32(void *p) {
    uintptr_t k = (uintptr_t)p;
    return ((uint32_t)(k >> 4) ^ (uint32_t)(k >> 13)) * 2654435761u;
}

// Compact open-addressed linear-probing map keyed by non-null COM pointers (0 = empty, 1 = tombstone).
template <class V> class PtrMap {
    struct Slot {
        uintptr_t key;
        V val;
    };
    Slot *slots_ = nullptr;
    size_t mask_ = 0;
    size_t count_ = 0;
    size_t used_ = 0;

    void rehash(size_t new_cap) {
        Slot *ns = new Slot[new_cap]();
        size_t nm = new_cap - 1;
        if (slots_) {
            size_t old_cap = mask_ + 1;
            for (size_t i = 0; i < old_cap; ++i) {
                uintptr_t k = slots_[i].key;
                if (k <= 1) continue;
                size_t idx = (size_t)ptr_hash32((void *)k) & nm;
                while (ns[idx].key != 0) idx = (idx + 1) & nm;
                ns[idx].key = k;
                ns[idx].val = slots_[i].val;
            }
            delete[] slots_;
        }
        slots_ = ns;
        mask_ = nm;
        used_ = count_;
    }

public:
    V *find(void *p) const {
        if (!count_) return nullptr;
        uintptr_t k = (uintptr_t)p;
        if (k <= 1) return nullptr;
        size_t idx = (size_t)ptr_hash32(p) & mask_;
        for (;;) {
            uintptr_t sk = slots_[idx].key;
            if (sk == k) return &slots_[idx].val;
            if (sk == 0) return nullptr;
            idx = (idx + 1) & mask_;
        }
    }

    void insert(void *p, const V &v) {
        uintptr_t k = (uintptr_t)p;
        if (k <= 1) return;
        if (!slots_) rehash(64);
        else if ((used_ + 1) * 10 >= (mask_ + 1) * 7)
            rehash(count_ * 2 >= (mask_ + 1) ? (mask_ + 1) * 2 : (mask_ + 1));
        size_t idx = (size_t)ptr_hash32(p) & mask_;
        size_t tomb = (size_t)-1;
        for (;;) {
            uintptr_t sk = slots_[idx].key;
            if (sk == k) {
                slots_[idx].val = v;
                return;
            }
            if (sk == 1) {
                if (tomb == (size_t)-1) tomb = idx;
            } else if (sk == 0) {
                size_t dst = tomb != (size_t)-1 ? tomb : idx;
                if (dst == idx) ++used_;
                slots_[dst].key = k;
                slots_[dst].val = v;
                ++count_;
                return;
            }
            idx = (idx + 1) & mask_;
        }
    }

    bool erase(void *p) {
        if (!count_) return false;
        uintptr_t k = (uintptr_t)p;
        if (k <= 1) return false;
        size_t idx = (size_t)ptr_hash32(p) & mask_;
        for (;;) {
            uintptr_t sk = slots_[idx].key;
            if (sk == k) {
                slots_[idx].key = 1;
                slots_[idx].val = V{};
                --count_;
                return true;
            }
            if (sk == 0) return false;
            idx = (idx + 1) & mask_;
        }
    }

    size_t size() const { return count_; }

    template <class Fn> void for_each(Fn &&fn) const {
        if (!slots_ || !count_) return;
        size_t cap = mask_ + 1;
        for (size_t i = 0; i < cap; ++i)
            if (slots_[i].key > 1) fn((void *)slots_[i].key, slots_[i].val);
    }
};

// ---------------------------------------------------------------- vtable registry
// Hooks are installed into the vtables the runtime gives out. Each hooked vtable keeps the
// entries it had, so a hook calls the original for the object's own vtable.
enum VtKind { VT_D3D, VT_DEV, VT_TEX, VT_CUBE, VT_VOL, VT_SURF, VT_VB, VT_IB, VT_KINDS };
struct VtEntry {
    void **vt;
    std::atomic<void *> orig[128];
};
static std::vector<VtEntry *> g_vts[VT_KINDS];
static std::atomic<VtEntry *> g_vt_primary[VT_KINDS];
static SRWLOCK g_vt_lock = SRWLOCK_INIT;

static VtEntry *vt_entry_locked(VtKind kind, void **vt, bool create) {
    for (VtEntry *e : g_vts[kind])
        if (e->vt == vt) return e;
    if (!create) return nullptr;
    VtEntry *e = new VtEntry();
    e->vt = vt;
    for (int i = 0; i < 128; ++i) e->orig[i].store(nullptr, std::memory_order_relaxed);
    bool first = g_vts[kind].empty();
    g_vts[kind].push_back(e);
    if (first) g_vt_primary[kind].store(e, std::memory_order_release);
    return e;
}

static void hook(VtKind kind, void *obj, int index, void *fn) {
    if (index < 0 || index >= 128) return;
    void **vt = *(void ***)obj;
    VtEntry *p = g_vt_primary[kind].load(std::memory_order_acquire);
    if (p && p->vt == vt && vt[index] == fn && p->orig[index].load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_vt_lock);
    VtEntry *e = vt_entry_locked(kind, vt, true);
    if (vt[index] == fn) {
        if (!e->orig[index].load(std::memory_order_relaxed)) {
            // already ours through another entry: copy that entry's original
            for (VtEntry *o : g_vts[kind]) {
                void *ofn = o->orig[index].load(std::memory_order_relaxed);
                if (o != e && ofn) { e->orig[index].store(ofn, std::memory_order_release); break; }
            }
        }
    } else {
        // Unhooked slot, reused heap vtable address (device recreation), or runtime vtable rewrite
        // (e.g. BeginStateBlock/EndStateBlock): re-patch and refresh the stored original.
        void *old = patch_vtable(vt, index, fn);
        if (old && old != fn) e->orig[index].store(old, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_vt_lock);
}

static void save_orig(VtKind kind, void *obj, int index) {
    if (index < 0 || index >= 128) return;
    void **vt = *(void ***)obj;
    AcquireSRWLockExclusive(&g_vt_lock);
    VtEntry *e = vt_entry_locked(kind, vt, true);
    if (!e->orig[index].load(std::memory_order_relaxed)) e->orig[index].store(vt[index], std::memory_order_release);
    ReleaseSRWLockExclusive(&g_vt_lock);
}

static __declspec(noinline) void *orig_slow(VtKind kind, void **vt, int index) {
    AcquireSRWLockShared(&g_vt_lock);
    VtEntry *e = vt_entry_locked(kind, vt, false);
    void *fn = e ? e->orig[index].load(std::memory_order_acquire) : nullptr;
    if (!fn) {
        // An object whose vtable we never saw (copied by an overlay): use any original we have.
        for (VtEntry *o : g_vts[kind]) {
            void *ofn = o->orig[index].load(std::memory_order_acquire);
            if (ofn) { fn = ofn; break; }
        }
    }
    unsigned count = fn ? 0 : (unsigned)g_vts[kind].size();
    ReleaseSRWLockShared(&g_vt_lock);
    if (fn) return fn;
    log("internal: no original for vtable %p kind %d index %d (entry %p, %u vtables of the kind)", vt, kind,
        index, e, count);
    return nullptr;
}

template <class F> static __forceinline F orig(VtKind kind, void *obj, int index) {
    if (index < 0 || index >= 128) return nullptr;
    void **vt = *(void ***)obj;
    VtEntry *p = g_vt_primary[kind].load(std::memory_order_acquire);
    if (p && p->vt == vt) {
        void *fn = p->orig[index].load(std::memory_order_acquire);
        if (fn) return (F)fn;
    }
    return (F)orig_slow(kind, vt, index);
}

// ---------------------------------------------------------------- statistics
static std::atomic<long long> s_tex_count{0}, s_tex_bytes{0}, s_buf_count{0}, s_buf_bytes{0};
static std::atomic<long long> s_tex_total{0}, s_buf_total{0};
static std::atomic<long> s_lock_full{0}, s_lock_partial{0}, s_lock_read{0}, s_lock_fail{0}, s_upload_fail{0};
static std::atomic<long> s_create_fail{0};
static long long s_shadow_bytes = 0;                       // rewritten textures' system-memory copies (g_cs)
static long s_shadow_count = 0, s_coalesced = 0, s_flushes = 0;
static std::atomic<bool> s_device_ex{false};
static std::atomic<bool> s_multithreaded{false};

static UINT format_bits(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_DXT1: return 4;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 8;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: return 8;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:
    case D3DFMT_V8U8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_A8P8: case D3DFMT_L6V5U5: return 16;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return 64;
    case D3DFMT_A32B32G32R32F: return 128;
    default: return 32;
    }
}
static bool is_dxt(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}
static long long surface_bytes(D3DFORMAT f, UINT w, UINT h) {
    if (is_dxt(f)) { w = (w + 3) & ~3u; h = (h + 3) & ~3u; }
    return (long long)w * h * format_bits(f) / 8;
}
static long long chain_bytes(D3DFORMAT f, UINT w, UINT h, UINT levels) {
    long long total = 0;
    if (levels == 0) levels = 32;
    for (UINT l = 0; l < levels; ++l) {
        total += surface_bytes(f, w, h);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

// ---------------------------------------------------------------- offloaded resources
struct Upload {                       // one open lock on a texture level or cube face level
    IDirect3DTexture9 *staging;
    D3DFORMAT format;
    UINT staging_w, staging_h;
    UINT staging_level;
    RECT rect;
    bool has_rect;
    DWORD flags;
};
struct UploadEntry {
    UINT key;
    Upload u;
};
struct Res {
    VtKind kind;
    IDirect3DDevice9 *dev;
    D3DFORMAT format;
    UINT levels;
    long long bytes;
    std::vector<UploadEntry> uploads;           // key: face << 16 | level (almost always 0 or 1 item)
    std::vector<void *> surfaces;
    IDirect3DVolumeTexture9 *vol_shadow;        // volume textures: persistent SYSTEMMEM copy
    IUnknown *sys_copy;                         // keep_system_copy (test): what the managed pool would hold
    UINT w, h;                                  // top level size
    long upload_count;                          // write locks so far
    // A texture rewritten over and over (particle sheets, dynamic lightmaps) keeps a system-memory copy,
    // as the managed pool did: locks write into it and the changed rectangles go to video memory once,
    // before the texture is next drawn with (see "rewritten textures" below).
    IDirect3DTexture9 *shadow;
    IDirect3DSurface9 *shadow_surf[16];         // cached shadow->GetSurfaceLevel(level) (owned by shadow, released before shadow)
    bool shadow_failed;                         // readback unsupported: keep using staging uploads
    bool in_dirty;                              // present in g_dirty
    UINT open_mask;                             // shadow levels locked now
    RECT lock_rect[16];                         // their lock rectangles
    bool lock_full[16], lock_write[16];
    UINT dirty_mask;                            // shadow levels with changes not yet uploaded
    RECT dirty[16];
    bool dirty_full[16];
};
static PtrMap<Res *> g_res;                     // texture -> state
struct SurfRef { void *tex; UINT face, level; };
static PtrMap<SurfRef> g_surfs;                 // surface of an offloaded texture
static std::atomic<long> s_surf_count{0};
static PtrMap<long long> g_bufs;                // offloaded vertex/index buffer -> bytes
static PtrMap<IUnknown *> g_buf_copies;         // keep_system_copy (test)
static CRITICAL_SECTION g_cs;

// Lock-free counting Bloom filter for g_bufs so dynamic D3DPOOL_DEFAULT VBs/IBs skip g_cs on Lock/Release.
static const uint32_t kBufBloomMask = 16383;
static std::atomic<uint8_t> g_buf_cnt[16384];
static void buf_bloom_add(void *b) {
    uint32_t h = ptr_hash32(b);
    uint32_t i1 = h & kBufBloomMask, i2 = (h >> 14) & kBufBloomMask;
    uint8_t c1 = g_buf_cnt[i1].load(std::memory_order_relaxed);
    if (c1 < 255) g_buf_cnt[i1].store((uint8_t)(c1 + 1), std::memory_order_release);
    if (i2 != i1) {
        uint8_t c2 = g_buf_cnt[i2].load(std::memory_order_relaxed);
        if (c2 < 255) g_buf_cnt[i2].store((uint8_t)(c2 + 1), std::memory_order_release);
    }
}
static void buf_bloom_remove(void *b) {
    uint32_t h = ptr_hash32(b);
    uint32_t i1 = h & kBufBloomMask, i2 = (h >> 14) & kBufBloomMask;
    uint8_t c1 = g_buf_cnt[i1].load(std::memory_order_relaxed);
    if (c1 > 0 && c1 < 255) g_buf_cnt[i1].store((uint8_t)(c1 - 1), std::memory_order_release);
    if (i2 != i1) {
        uint8_t c2 = g_buf_cnt[i2].load(std::memory_order_relaxed);
        if (c2 > 0 && c2 < 255) g_buf_cnt[i2].store((uint8_t)(c2 - 1), std::memory_order_release);
    }
}
static __forceinline bool buf_bloom_maybe(void *b) {
    uint32_t h = ptr_hash32(b);
    return g_buf_cnt[h & kBufBloomMask].load(std::memory_order_acquire) != 0 &&
           g_buf_cnt[(h >> 14) & kBufBloomMask].load(std::memory_order_acquire) != 0;
}

struct CsGuard {
    CsGuard() { EnterCriticalSection(&g_cs); }
    ~CsGuard() { LeaveCriticalSection(&g_cs); }
};

static __forceinline Res *find_res(void *tex) {
    Res **p = g_res.find(tex);
    return p ? *p : nullptr;
}

static std::atomic<void *> g_bound[20];            // texture per sampler (0-15, vertex samplers 16-19)
static std::atomic<uint32_t> g_bound_dirty_mask{0}; // sampler slot bits whose bound texture has unuploaded changes
static std::vector<void *> g_dirty;                // textures with changes not yet uploaded (g_cs)
static std::atomic<long> g_dirty_count{0};         // number of textures with dirty_mask != 0
static std::atomic<uint32_t> g_dirty_bits[4];      // 128-bit exact bloom for g_dirty (rebuilt under g_cs)
static IDirect3DDevice9 *g_active_dev = nullptr;   // active 9Ex device (g_cs)

static void dirty_bloom_rebuild() {
    uint32_t b[4] = {};
    for (void *t : g_dirty) {
        uint32_t h = ptr_hash32(t);
        b[(h >> 27) & 3] |= 1u << ((h >> 22) & 31);
    }
    for (int i = 0; i < 4; ++i) g_dirty_bits[i].store(b[i], std::memory_order_release);
}
static __forceinline bool dirty_bloom_maybe(void *t) {
    uint32_t h = ptr_hash32(t);
    return (g_dirty_bits[(h >> 27) & 3].load(std::memory_order_acquire) & (1u << ((h >> 22) & 31))) != 0;
}

static void dirty_remove(void *tex) {
    for (size_t i = 0; i < g_dirty.size(); ++i) {
        if (g_dirty[i] == tex) {
            g_dirty[i] = g_dirty.back();
            g_dirty.pop_back();
            return;
        }
    }
}

static void forget_res(void *tex) {
    Res **slot = g_res.find(tex);
    if (!slot) return;
    Res *r = *slot;
    uint32_t clear_mask = 0;
    for (int i = 0; i < 20; ++i) {
        void *expected = tex;
        if (g_bound[i].compare_exchange_strong(expected, nullptr, std::memory_order_relaxed))
            clear_mask |= 1u << i;
    }
    if (clear_mask) g_bound_dirty_mask.fetch_and(~clear_mask, std::memory_order_release);
    if (r->in_dirty) {
        r->in_dirty = false;
        dirty_remove(tex);
        dirty_bloom_rebuild();
    }
    if (r->dirty_mask) g_dirty_count.fetch_sub(1, std::memory_order_release);
    for (void *s : r->surfaces) {
        if (g_surfs.erase(s)) s_surf_count.fetch_sub(1, std::memory_order_relaxed);
    }
    for (auto &e : r->uploads) if (e.u.staging) e.u.staging->Release();
    for (int i = 0; i < 16; ++i) {
        if (r->shadow_surf[i]) { r->shadow_surf[i]->Release(); r->shadow_surf[i] = nullptr; }
    }
    if (r->shadow) { r->shadow->Release(); s_shadow_bytes -= r->bytes; s_shadow_count--; }
    if (r->vol_shadow) r->vol_shadow->Release();
    if (r->sys_copy) r->sys_copy->Release();
    s_tex_count--;
    s_tex_bytes -= r->bytes;
    delete r;
    g_res.erase(tex);
}

// ---------------------------------------------------------------- function types
typedef HRESULT(WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT(WINAPI *CreateTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT(WINAPI *CreateVolumeTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *);
typedef HRESULT(WINAPI *CreateCubeTexture_t)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *);
typedef HRESULT(WINAPI *CreateVertexBuffer_t)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
typedef HRESULT(WINAPI *CreateIndexBuffer_t)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
typedef HRESULT(WINAPI *UpdateSurface_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *);
typedef HRESULT(WINAPI *UpdateTexture_t)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
typedef HRESULT(WINAPI *StretchRect_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
typedef HRESULT(WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef ULONG(WINAPI *Release_t)(IUnknown *);
typedef HRESULT(WINAPI *TexGetSurfaceLevel_t)(IDirect3DTexture9 *, UINT, IDirect3DSurface9 **);
typedef HRESULT(WINAPI *TexLockRect_t)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT(WINAPI *TexUnlockRect_t)(IDirect3DTexture9 *, UINT);
typedef HRESULT(WINAPI *TexAddDirtyRect_t)(IDirect3DTexture9 *, const RECT *);
typedef HRESULT(WINAPI *CubeGetSurface_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, IDirect3DSurface9 **);
typedef HRESULT(WINAPI *CubeLockRect_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT(WINAPI *CubeUnlockRect_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT);
typedef HRESULT(WINAPI *CubeAddDirtyRect_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, const RECT *);
typedef HRESULT(WINAPI *VolLockBox_t)(IDirect3DVolumeTexture9 *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD);
typedef HRESULT(WINAPI *VolUnlockBox_t)(IDirect3DVolumeTexture9 *, UINT);
typedef HRESULT(WINAPI *VolAddDirtyBox_t)(IDirect3DVolumeTexture9 *, const D3DBOX *);
typedef HRESULT(WINAPI *SurfLockRect_t)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT(WINAPI *SurfUnlockRect_t)(IDirect3DSurface9 *);
typedef HRESULT(WINAPI *BufLock_t)(void *, UINT, UINT, void **, DWORD);

// device vtable indices
enum { D_RELEASE = 2, D_RESET = 16, D_PRESENT = 17, D_CREATETEXTURE = 23, D_CREATEVOLUME = 24, D_CREATECUBE = 25,
       D_CREATEVB = 26, D_CREATEIB = 27, D_UPDATESURFACE = 30, D_UPDATETEXTURE = 31, D_STRETCHRECT = 34 };
// texture / cube / volume texture vtable indices
enum { T_RELEASE = 2, T_GETSURFACE = 18, T_LOCK = 19, T_UNLOCK = 20, T_ADDDIRTY = 21 };
// surface vtable indices
enum { S_RELEASE = 2, S_LOCK = 13, S_UNLOCK = 14 };
// vertex / index buffer vtable indices
enum { B_RELEASE = 2, B_LOCK = 11 };

// ---------------------------------------------------------------- staging (upload) textures
struct StagingKey { IDirect3DDevice9 *dev; D3DFORMAT format; UINT w, h, levels; };
struct StagingItem { StagingKey key; IDirect3DTexture9 *tex; long long bytes; };
static std::deque<StagingItem> g_staging_cache;
static long long g_staging_bytes = 0;
static __declspec(thread) bool g_in_dev_release = false;
static int g_staging_adds_dev_ref = -1;   // -1 unknown, 0 internal-ref (wrappers), 1 COM-ref (native d3d9)

// A SYSTEMMEM texture whose level `*level` has exactly w x h. DXT textures cannot be created
// smaller than 4x4 on all drivers, so tiny DXT mip levels use a larger texture's lower level.
static IDirect3DTexture9 *staging_get(IDirect3DDevice9 *dev, D3DFORMAT format, UINT w, UINT h, UINT *level) {
    UINT k = 0;
    if (is_dxt(format))
        while (((w << k) & 3) || ((h << k) & 3)) ++k;
    StagingKey key = {dev, format, w << k, h << k, k + 1};
    *level = k;
    for (size_t i = g_staging_cache.size(); i-- > 0;) {
        StagingItem &it = g_staging_cache[i];
        if (it.key.dev == key.dev && it.key.format == key.format && it.key.w == key.w && it.key.h == key.h && it.key.levels == key.levels) {
            IDirect3DTexture9 *t = it.tex;
            g_staging_bytes -= it.bytes;
            if (i + 1 < g_staging_cache.size()) it = g_staging_cache.back();
            g_staging_cache.pop_back();
            return t;
        }
    }
    IDirect3DTexture9 *t = nullptr;
    auto create = orig<CreateTexture_t>(VT_DEV, dev, D_CREATETEXTURE);
    ULONG before = 0;
    bool probe_ref = (g_staging_adds_dev_ref < 0);
    if (probe_ref) {
        g_in_dev_release = true;
        dev->AddRef();
        before = dev->Release();
        g_in_dev_release = false;
    }
    HRESULT hr = create(dev, key.w, key.h, key.levels, 0, format, D3DPOOL_SYSTEMMEM, &t, nullptr);
    if (FAILED(hr)) {
        log("upload texture %ux%u format %08x levels %u: CreateTexture failed %08lx", key.w, key.h, format, key.levels, hr);
        return nullptr;
    }
    if (probe_ref && t) {
        g_in_dev_release = true;
        dev->AddRef();
        ULONG after = dev->Release();
        g_in_dev_release = false;
        g_staging_adds_dev_ref = (after > before) ? 1 : 0;
    }
    return t;
}

static void staging_put(IDirect3DDevice9 *dev, IDirect3DTexture9 *t, D3DFORMAT format, UINT w, UINT h, UINT levels) {
    long long bytes = chain_bytes(format, w, h, levels);
    long long cap = (long long)g_settings.staging_cache_mb << 20;
    if (bytes > cap / 4) { t->Release(); return; }   // big one-off uploads are not kept
    g_staging_cache.push_back({{dev, format, w, h, levels}, t, bytes});
    g_staging_bytes += bytes;
    while (g_staging_bytes > cap && !g_staging_cache.empty()) {
        g_staging_bytes -= g_staging_cache.front().bytes;
        g_staging_cache.front().tex->Release();
        g_staging_cache.pop_front();
    }
}

// ---------------------------------------------------------------- lock emulation
// Upload textures are called through their own interface, not through orig(): the 2D texture
// hooks may not exist yet (the game's first managed resource can be a cube map), and the hooks
// pass textures that are not offloaded straight through.
static HRESULT level_desc(Res *r, void *tex, UINT level, D3DSURFACE_DESC *d) {
    // Not hooked: the pool must read DEFAULT, the runtime validates StretchRect/UpdateSurface through it.
    if (r->kind == VT_TEX) return ((IDirect3DTexture9 *)tex)->GetLevelDesc(level, d);
    return ((IDirect3DCubeTexture9 *)tex)->GetLevelDesc(level, d);
}

static HRESULT level_surface(Res *r, void *tex, UINT face, UINT level, IDirect3DSurface9 **s) {
    if (r->kind == VT_TEX) return orig<TexGetSurfaceLevel_t>(VT_TEX, tex, T_GETSURFACE)((IDirect3DTexture9 *)tex, level, s);
    return orig<CubeGetSurface_t>(VT_CUBE, tex, T_GETSURFACE)((IDirect3DCubeTexture9 *)tex, (D3DCUBEMAP_FACES)face, level, s);
}

// ---------------------------------------------------------------- rewritten textures
static const long kShadowAfterLocks = 32;          // write locks before a texture gets its copy
static const long long kShadowMaxBytes = 1 << 20;  // per texture
static const long long kShadowCapBytes = 64ll << 20;

static __forceinline int sampler_slot(DWORD stage) {
    if (stage < 16) return (int)stage;
    if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) return 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}
static uint32_t bound_slots_mask(void *tex) {
    uint32_t mask = 0;
    for (int i = 0; i < 20; ++i)
        if (g_bound[i].load(std::memory_order_relaxed) == tex) mask |= 1u << i;
    return mask;
}

static bool copy_level_to_shadow(Res *r, void *tex, IDirect3DTexture9 *shadow, UINT level) {
    UINT lw = r->w >> level, lh = r->h >> level;
    D3DFORMAT lfmt = r->format;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    if (r->w == 0 || r->h == 0) {
        D3DSURFACE_DESC d;
        if (FAILED(level_desc(r, tex, level, &d))) return false;
        lw = d.Width; lh = d.Height; lfmt = d.Format;
    }
    IDirect3DSurface9 *src = nullptr, *rt = nullptr, *dst = nullptr;
    auto stretch = orig<StretchRect_t>(VT_DEV, r->dev, D_STRETCHRECT);
    bool ok = false;
    if (stretch && SUCCEEDED(level_surface(r, tex, 0, level, &src)) &&
        SUCCEEDED(r->dev->CreateRenderTarget(lw, lh, lfmt, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
        SUCCEEDED(shadow->GetSurfaceLevel(level, &dst)) &&
        SUCCEEDED(stretch(r->dev, src, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
        SUCCEEDED(r->dev->GetRenderTargetData(rt, dst))) {
        ok = true;
    }
    if (dst) dst->Release();
    if (rt) rt->Release();
    if (src) src->Release();
    return ok;
}

static void make_shadow(Res *r, void *tex, UINT lock_level, const RECT *lock_rect) {
    if (r->kind != VT_TEX || r->shadow || r->shadow_failed || !r->uploads.empty() || r->levels > 16) return;
    if (r->bytes > kShadowMaxBytes || s_shadow_bytes + r->bytes > kShadowCapBytes) return;
    IDirect3DTexture9 *t = nullptr;
    auto create = orig<CreateTexture_t>(VT_DEV, r->dev, D_CREATETEXTURE);
    if (FAILED(create(r->dev, r->w, r->h, r->levels, 0, r->format, D3DPOOL_SYSTEMMEM, &t, nullptr))) {
        r->shadow_failed = true;
        return;
    }
    // Initialize the shadow with whatever the first kShadowAfterLocks writes put in VRAM; otherwise a later
    // coalesced dirty-rect upload overwrites untouched areas inside the bounding box with zeros (black lightmaps).
    bool full_single = r->levels == 1 && lock_level == 0 &&
                       (!lock_rect || (lock_rect->left == 0 && lock_rect->top == 0 &&
                                       lock_rect->right == (LONG)r->w && lock_rect->bottom == (LONG)r->h));
    for (UINT l = 0; l < r->levels; ++l) {
        if (!copy_level_to_shadow(r, tex, t, l)) {
            if (full_single) break;   // single-level full overwrite does not depend on previous contents
            t->Release();
            r->shadow_failed = true;
            if (g_settings.verbose)
                log("shadow: readback unsupported for %ux%u format %08x levels %u; keeping direct uploads",
                    r->w, r->h, r->format, r->levels);
            return;
        }
    }
    r->shadow = t;
    s_shadow_bytes += r->bytes;
    s_shadow_count++;
}

// Uploads a texture's changed rectangles (g_cs held).
static void flush_shadow(Res *r, void *tex) {
    if (!r->dirty_mask) return;
    auto update = orig<UpdateSurface_t>(VT_DEV, r->dev, D_UPDATESURFACE);
    bool uploaded = false;
    for (UINT level = 0; level < r->levels && r->dirty_mask; ++level) {
        UINT bit = 1u << level;
        if (!(r->dirty_mask & bit) || (r->open_mask & bit)) continue;
        r->dirty_mask &= ~bit;
        IDirect3DSurface9 *src = r->shadow_surf[level];
        if (!src) {
            if (SUCCEEDED(r->shadow->GetSurfaceLevel(level, &src))) r->shadow_surf[level] = src;
        }
        IDirect3DSurface9 *dst = nullptr;
        level_surface(r, tex, 0, level, &dst);
        if (src && dst) {
            HRESULT hr;
            if (r->dirty_full[level]) hr = update(r->dev, src, nullptr, dst, nullptr);
            else {
                POINT pt = {r->dirty[level].left, r->dirty[level].top};
                hr = update(r->dev, src, &r->dirty[level], dst, &pt);
            }
            if (FAILED(hr) && ++s_upload_fail <= 10) log("UpdateSurface (rewritten texture) failed %08lx level %u", hr, level);
            uploaded = true;
        } else if (++s_upload_fail <= 10) {
            log("flush_shadow: GetSurfaceLevel failed level %u (src %p dst %p)", level, (void *)src, (void *)dst);
        }
        if (dst) dst->Release();
    }
    if (!r->dirty_mask) {
        if (r->in_dirty) {
            r->in_dirty = false;
            dirty_remove(tex);
            dirty_bloom_rebuild();
        }
        g_dirty_count.fetch_sub(1, std::memory_order_release);
    }
    if (uploaded) s_flushes++;
}

static void flush_bound() {
    uint32_t mask = g_bound_dirty_mask.load(std::memory_order_relaxed);
    uint32_t still_dirty = 0;
    unsigned long slot = 0;
    while (_BitScanForward(&slot, mask)) {
        mask &= mask - 1;
        void *b = g_bound[slot].load(std::memory_order_relaxed);
        if (!b) continue;
        Res *r = find_res(b);
        if (r && r->dirty_mask) {
            flush_shadow(r, b);
            if (r->dirty_mask & ~r->open_mask) still_dirty |= 1u << slot;
        }
    }
    g_bound_dirty_mask.store(still_dirty, std::memory_order_release);
}

static bool valid_lock_rect(const RECT *r, UINT w, UINT h) {
    return !r || (r->left >= 0 && r->top >= 0 && r->right > r->left && r->bottom > r->top &&
                  (UINT)r->right <= w && (UINT)r->bottom <= h);
}

static HRESULT shadow_lock(Res *r, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
    if (level >= r->levels || level >= 16) { s_lock_fail++; return D3DERR_INVALIDCALL; }
    UINT bit = 1u << level;
    if (r->open_mask & bit) { s_lock_fail++; return D3DERR_INVALIDCALL; }
    UINT w = r->w >> level, h = r->h >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!valid_lock_rect(rect, w, h)) { s_lock_fail++; return D3DERR_INVALIDCALL; }
    HRESULT hr = r->shadow->LockRect(level, out, rect, flags & ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
    if (FAILED(hr)) { s_lock_fail++; return hr; }
    r->open_mask |= bit;
    r->lock_full[level] = !rect || (rect->left == 0 && rect->top == 0 && rect->right == (LONG)w && rect->bottom == (LONG)h);
    if (rect) r->lock_rect[level] = *rect;
    r->lock_write[level] = !(flags & D3DLOCK_READONLY);
    if (!r->lock_write[level]) s_lock_read++;
    else if (r->lock_full[level]) s_lock_full++;
    else s_lock_partial++;
    if (r->lock_write[level]) r->upload_count++;
    return D3D_OK;
}

static HRESULT shadow_unlock(Res *r, void *tex, UINT level) {
    if (level >= r->levels || level >= 16) return D3DERR_INVALIDCALL;
    UINT bit = 1u << level;
    if (!(r->open_mask & bit)) return D3DERR_INVALIDCALL;
    r->open_mask &= ~bit;
    r->shadow->UnlockRect(level);
    if (!r->lock_write[level]) {
        if (r->dirty_mask & ~r->open_mask) {
            uint32_t bm = bound_slots_mask(tex);
            if (bm) g_bound_dirty_mask.fetch_or(bm, std::memory_order_release);
        }
        return D3D_OK;
    }
    if (!r->dirty_mask) {
        if (!r->in_dirty) {
            r->in_dirty = true;
            g_dirty.push_back(tex);
            dirty_bloom_rebuild();
        }
        g_dirty_count.fetch_add(1, std::memory_order_release);
    }
    if (r->dirty_mask & bit) {
        s_coalesced++;   // a second change before the texture was used: one upload for both
        if (!r->dirty_full[level]) {
            if (r->lock_full[level]) r->dirty_full[level] = true;
            else {
                RECT &d = r->dirty[level];
                const RECT &l = r->lock_rect[level];
                if (l.left < d.left) d.left = l.left;
                if (l.top < d.top) d.top = l.top;
                if (l.right > d.right) d.right = l.right;
                if (l.bottom > d.bottom) d.bottom = l.bottom;
            }
        }
    } else {
        r->dirty_full[level] = r->lock_full[level];
        r->dirty[level] = r->lock_rect[level];
    }
    r->dirty_mask |= bit;
    uint32_t bm = bound_slots_mask(tex);
    if (bm) g_bound_dirty_mask.fetch_or(bm, std::memory_order_release);
    return D3D_OK;
}

static HRESULT emu_lock(Res *r, void *tex, UINT face, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
    if (!out) return D3DERR_INVALIDCALL;
    if (r->kind == VT_CUBE && face >= 6) { s_lock_fail++; return D3DERR_INVALIDCALL; }
    if (r->levels > 0 && level >= r->levels) { s_lock_fail++; return D3DERR_INVALIDCALL; }
    UINT lw = 0, lh = 0;
    D3DFORMAT lfmt = r->format;
    if (r->w > 0 && r->h > 0 && level < r->levels) {
        lw = r->w >> level; if (!lw) lw = 1;
        lh = r->h >> level; if (!lh) lh = 1;
    } else {
        D3DSURFACE_DESC d;
        HRESULT hr = level_desc(r, tex, level, &d);
        if (FAILED(hr)) { s_lock_fail++; return hr; }
        lw = d.Width; lh = d.Height; lfmt = d.Format;
    }
    if (!valid_lock_rect(rect, lw, lh)) { s_lock_fail++; return D3DERR_INVALIDCALL; }
    if (r->kind == VT_TEX && !r->shadow && r->upload_count >= kShadowAfterLocks && !(flags & D3DLOCK_READONLY))
        make_shadow(r, tex, level, rect);
    if (r->shadow) return shadow_lock(r, level, out, rect, flags);
    UINT key = face << 16 | level;
    for (const auto &e : r->uploads)
        if (e.key == key) { s_lock_fail++; return D3DERR_INVALIDCALL; }
    Upload u = {};
    u.format = lfmt;
    u.flags = flags;
    if (rect) {
        u.rect = *rect;
        u.has_rect = !(rect->left == 0 && rect->top == 0 && rect->right == (LONG)lw && rect->bottom == (LONG)lh);
    }
    if (flags & D3DLOCK_READONLY) {
        if (++s_lock_read <= 5) log("note: read-only lock on an offloaded texture (format %08x, %ux%u level %u); contents are not read back", lfmt, lw, lh, level);
    } else if (u.has_rect) s_lock_partial++;
    else s_lock_full++;
    u.staging = staging_get(r->dev, lfmt, lw, lh, &u.staging_level);
    if (!u.staging) { s_lock_fail++; return D3DERR_OUTOFVIDEOMEMORY; }
    u.staging_w = lw << u.staging_level;
    u.staging_h = lh << u.staging_level;
    HRESULT hr = u.staging->LockRect(u.staging_level, out, rect, flags & ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
    if (FAILED(hr)) {
        s_lock_fail++;
        staging_put(r->dev, u.staging, u.format, u.staging_w, u.staging_h, u.staging_level + 1);
        return hr;
    }
    r->uploads.push_back({key, u});
    if (!(flags & D3DLOCK_READONLY) && ++r->upload_count == 3000 && g_settings.verbose) {
        // a texture rewritten this often: say which code does it (once per texture)
        char callers[700];
        scan_stack((void **)_AddressOfReturnAddress(), callers, sizeof(callers), 10);
        log("lock: %ux%u format %08x level %u flags %08lx rect %d locked 3000 times; callers: %s", lw, lh,
            lfmt, level, flags, rect != nullptr, callers);
    }
    return D3D_OK;
}

static HRESULT emu_unlock(Res *r, void *tex, UINT face, UINT level) {
    if (r->shadow) return shadow_unlock(r, tex, level);
    UINT key = face << 16 | level;
    size_t idx = (size_t)-1;
    for (size_t i = 0; i < r->uploads.size(); ++i)
        if (r->uploads[i].key == key) { idx = i; break; }
    if (idx == (size_t)-1) return D3DERR_INVALIDCALL;
    Upload u = r->uploads[idx].u;
    r->uploads[idx] = r->uploads.back();
    r->uploads.pop_back();
    u.staging->UnlockRect(u.staging_level);
    if (!(u.flags & D3DLOCK_READONLY)) {
        IDirect3DSurface9 *src = nullptr, *dst = nullptr;
        u.staging->GetSurfaceLevel(u.staging_level, &src);
        level_surface(r, tex, face, level, &dst);
        if (src && dst) {
            POINT pt = {u.rect.left, u.rect.top};
            auto update = orig<UpdateSurface_t>(VT_DEV, r->dev, D_UPDATESURFACE);
            HRESULT hr = u.has_rect ? update(r->dev, src, &u.rect, dst, &pt) : update(r->dev, src, nullptr, dst, nullptr);
            if (FAILED(hr) && ++s_upload_fail <= 10)
                log("UpdateSurface failed %08lx (format %08x level %u face %u partial %d)", hr, r->format, level, face, u.has_rect);
        } else if (++s_upload_fail <= 10) {
            log("emu_unlock: GetSurfaceLevel failed (format %08x level %u face %u src %p dst %p)", r->format, level, face, (void *)src, (void *)dst);
        }
        if (src) src->Release();   // through the vtable: surface hooks may not exist yet
        if (dst) dst->Release();
    }
    staging_put(r->dev, u.staging, u.format, u.staging_w, u.staging_h, u.staging_level + 1);
    return D3D_OK;   // like the managed pool, an unlock does not fail
}

// ---------------------------------------------------------------- texture hooks
static void register_surface(Res *r, void *tex, UINT face, UINT level, IDirect3DSurface9 *s);

static ULONG WINAPI Tex_Release(IDirect3DTexture9 *t) {
    if (!s_multithreaded.load(std::memory_order_relaxed)) {
        ULONG n = orig<Release_t>(VT_TEX, t, T_RELEASE)(t);
        if (n == 0) { CsGuard g; forget_res(t); }
        return n;
    }
    CsGuard g;
    ULONG n = orig<Release_t>(VT_TEX, t, T_RELEASE)(t);
    if (n == 0) forget_res(t);
    return n;
}
static HRESULT WINAPI Tex_GetSurfaceLevel(IDirect3DTexture9 *t, UINT level, IDirect3DSurface9 **s) {
    HRESULT hr = orig<TexGetSurfaceLevel_t>(VT_TEX, t, T_GETSURFACE)(t, level, s);
    if (SUCCEEDED(hr) && s && *s) {
        CsGuard g;
        if (Res *r = find_res(t)) register_surface(r, t, 0, level, *s);
    }
    return hr;
}
static HRESULT WINAPI Tex_LockRect(IDirect3DTexture9 *t, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
    {
        CsGuard g;
        if (Res *r = find_res(t)) return emu_lock(r, t, 0, level, out, rect, flags);
    }
    return orig<TexLockRect_t>(VT_TEX, t, T_LOCK)(t, level, out, rect, flags);
}
static HRESULT WINAPI Tex_UnlockRect(IDirect3DTexture9 *t, UINT level) {
    {
        CsGuard g;
        if (Res *r = find_res(t)) return emu_unlock(r, t, 0, level);
    }
    return orig<TexUnlockRect_t>(VT_TEX, t, T_UNLOCK)(t, level);
}
static HRESULT WINAPI Tex_AddDirtyRect(IDirect3DTexture9 *t, const RECT *rect) {
    { CsGuard g; if (find_res(t)) return D3D_OK; }
    return orig<TexAddDirtyRect_t>(VT_TEX, t, T_ADDDIRTY)(t, rect);
}

static ULONG WINAPI Cube_Release(IDirect3DCubeTexture9 *t) {
    if (!s_multithreaded.load(std::memory_order_relaxed)) {
        ULONG n = orig<Release_t>(VT_CUBE, t, T_RELEASE)(t);
        if (n == 0) { CsGuard g; forget_res(t); }
        return n;
    }
    CsGuard g;
    ULONG n = orig<Release_t>(VT_CUBE, t, T_RELEASE)(t);
    if (n == 0) forget_res(t);
    return n;
}
static HRESULT WINAPI Cube_GetSurface(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **s) {
    HRESULT hr = orig<CubeGetSurface_t>(VT_CUBE, t, T_GETSURFACE)(t, face, level, s);
    if (SUCCEEDED(hr) && s && *s) { CsGuard g; if (Res *r = find_res(t)) register_surface(r, t, face, level, *s); }
    return hr;
}
static HRESULT WINAPI Cube_LockRect(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
    {
        CsGuard g;
        if (Res *r = find_res(t)) return emu_lock(r, t, face, level, out, rect, flags);
    }
    return orig<CubeLockRect_t>(VT_CUBE, t, T_LOCK)(t, face, level, out, rect, flags);
}
static HRESULT WINAPI Cube_UnlockRect(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT level) {
    {
        CsGuard g;
        if (Res *r = find_res(t)) return emu_unlock(r, t, face, level);
    }
    return orig<CubeUnlockRect_t>(VT_CUBE, t, T_UNLOCK)(t, face, level);
}
static HRESULT WINAPI Cube_AddDirtyRect(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, const RECT *rect) {
    { CsGuard g; if (find_res(t)) return D3D_OK; }
    return orig<CubeAddDirtyRect_t>(VT_CUBE, t, T_ADDDIRTY)(t, face, rect);
}

// Volume textures (rare): a persistent SYSTEMMEM copy takes the locks, UpdateTexture uploads.
static ULONG WINAPI Vol_Release(IDirect3DVolumeTexture9 *t) {
    if (!s_multithreaded.load(std::memory_order_relaxed)) {
        ULONG n = orig<Release_t>(VT_VOL, t, T_RELEASE)(t);
        if (n == 0) { CsGuard g; forget_res(t); }
        return n;
    }
    CsGuard g;
    ULONG n = orig<Release_t>(VT_VOL, t, T_RELEASE)(t);
    if (n == 0) forget_res(t);
    return n;
}
static HRESULT WINAPI Vol_LockBox(IDirect3DVolumeTexture9 *t, UINT level, D3DLOCKED_BOX *out, const D3DBOX *box, DWORD flags) {
    {
        CsGuard g;
        if (Res *r = find_res(t))
            if (r->vol_shadow)
                return orig<VolLockBox_t>(VT_VOL, r->vol_shadow, T_LOCK)(r->vol_shadow, level, out, box, flags & ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
    }
    return orig<VolLockBox_t>(VT_VOL, t, T_LOCK)(t, level, out, box, flags);
}
static HRESULT WINAPI Vol_UnlockBox(IDirect3DVolumeTexture9 *t, UINT level) {
    {
        CsGuard g;
        if (Res *r = find_res(t)) {
            if (r->vol_shadow) {
                HRESULT hr = orig<VolUnlockBox_t>(VT_VOL, r->vol_shadow, T_UNLOCK)(r->vol_shadow, level);
                if (SUCCEEDED(hr)) {
                    HRESULT uhr = orig<UpdateTexture_t>(VT_DEV, r->dev, D_UPDATETEXTURE)(r->dev, r->vol_shadow, t);
                    if (FAILED(uhr) && ++s_upload_fail <= 10)
                        log("UpdateTexture (volume) failed %08lx level %u", uhr, level);
                }
                return hr;
            }
        }
    }
    return orig<VolUnlockBox_t>(VT_VOL, t, T_UNLOCK)(t, level);
}
static HRESULT WINAPI Vol_AddDirtyBox(IDirect3DVolumeTexture9 *t, const D3DBOX *box) {
    {
        CsGuard g;
        if (Res *r = find_res(t))
            if (r->vol_shadow)
                return orig<VolAddDirtyBox_t>(VT_VOL, r->vol_shadow, T_ADDDIRTY)(r->vol_shadow, box);
    }
    return orig<VolAddDirtyBox_t>(VT_VOL, t, T_ADDDIRTY)(t, box);
}

// ---------------------------------------------------------------- surface hooks
static ULONG WINAPI Surf_Release(IDirect3DSurface9 *s) {
    if (!s_multithreaded.load(std::memory_order_relaxed)) {
        ULONG n = orig<Release_t>(VT_SURF, s, S_RELEASE)(s);
        if (n == 0 && s_surf_count.load(std::memory_order_relaxed) > 0) {
            CsGuard g;
            if (SurfRef *ref = g_surfs.find(s)) forget_res(ref->tex);
        }
        return n;
    }
    CsGuard g;
    ULONG n = orig<Release_t>(VT_SURF, s, S_RELEASE)(s);
    if (n == 0) {
        if (SurfRef *ref = g_surfs.find(s)) forget_res(ref->tex);
    }
    return n;
}
static HRESULT WINAPI Surf_LockRect(IDirect3DSurface9 *s, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
    if (s_surf_count.load(std::memory_order_relaxed) > 0) {
        CsGuard g;
        if (SurfRef *ref = g_surfs.find(s))
            if (Res *r = find_res(ref->tex)) return emu_lock(r, ref->tex, ref->face, ref->level, out, rect, flags);
    }
    return orig<SurfLockRect_t>(VT_SURF, s, S_LOCK)(s, out, rect, flags);
}
static HRESULT WINAPI Surf_UnlockRect(IDirect3DSurface9 *s) {
    if (s_surf_count.load(std::memory_order_relaxed) > 0) {
        CsGuard g;
        if (SurfRef *ref = g_surfs.find(s))
            if (Res *r = find_res(ref->tex)) return emu_unlock(r, ref->tex, ref->face, ref->level);
    }
    return orig<SurfUnlockRect_t>(VT_SURF, s, S_UNLOCK)(s);
}

static void register_surface(Res *r, void *tex, UINT face, UINT level, IDirect3DSurface9 *s) {
    if (g_surfs.find(s)) return;
    hook(VT_SURF, s, S_RELEASE, (void *)Surf_Release);
    hook(VT_SURF, s, S_LOCK, (void *)Surf_LockRect);
    hook(VT_SURF, s, S_UNLOCK, (void *)Surf_UnlockRect);
    g_surfs.insert(s, {tex, face, level});
    s_surf_count.fetch_add(1, std::memory_order_relaxed);
    r->surfaces.push_back(s);
}

// ---------------------------------------------------------------- buffer hooks
static void drop_buf_copy(void *b);
static void forget_buf_locked(void *b) {
    if (long long *bytes = g_bufs.find(b)) {
        s_buf_count--;
        s_buf_bytes -= *bytes;
        g_bufs.erase(b);
        buf_bloom_remove(b);
        drop_buf_copy(b);
    }
}
static ULONG WINAPI Vb_Release(IUnknown *b) {
    if (!s_multithreaded.load(std::memory_order_relaxed)) {
        ULONG n = orig<Release_t>(VT_VB, b, B_RELEASE)(b);
        if (n == 0 && buf_bloom_maybe(b)) { CsGuard g; forget_buf_locked(b); }
        return n;
    }
    CsGuard g;
    ULONG n = orig<Release_t>(VT_VB, b, B_RELEASE)(b);
    if (n == 0) forget_buf_locked(b);
    return n;
}
static ULONG WINAPI Ib_Release(IUnknown *b) {
    if (!s_multithreaded.load(std::memory_order_relaxed)) {
        ULONG n = orig<Release_t>(VT_IB, b, B_RELEASE)(b);
        if (n == 0 && buf_bloom_maybe(b)) { CsGuard g; forget_buf_locked(b); }
        return n;
    }
    CsGuard g;
    ULONG n = orig<Release_t>(VT_IB, b, B_RELEASE)(b);
    if (n == 0) forget_buf_locked(b);
    return n;
}
static void drop_buf_copy(void *b) {
    if (IUnknown **c = g_buf_copies.find(b)) {
        if (*c) (*c)->Release();
        g_buf_copies.erase(b);
    }
}

// DISCARD / NOOVERWRITE are only valid on dynamic buffers; the managed pool ignored them.
static HRESULT WINAPI Vb_Lock(void *b, UINT off, UINT size, void **data, DWORD flags) {
    if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) && buf_bloom_maybe(b)) {
        CsGuard g;
        if (g_bufs.find(b)) flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    }
    return orig<BufLock_t>(VT_VB, b, B_LOCK)(b, off, size, data, flags);
}
static HRESULT WINAPI Ib_Lock(void *b, UINT off, UINT size, void **data, DWORD flags) {
    if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) && buf_bloom_maybe(b)) {
        CsGuard g;
        if (g_bufs.find(b)) flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    }
    return orig<BufLock_t>(VT_IB, b, B_LOCK)(b, off, size, data, flags);
}

// ---------------------------------------------------------------- device hooks
static Res *new_res(VtKind kind, IDirect3DDevice9 *dev, D3DFORMAT format, UINT levels, long long bytes) {
    Res *r = new Res();
    r->kind = kind;
    r->dev = dev;
    r->format = format;
    r->levels = levels;
    r->bytes = bytes;
    r->vol_shadow = nullptr;
    r->sys_copy = nullptr;
    r->w = r->h = 0;
    r->upload_count = 0;
    s_tex_count++;
    s_tex_bytes += bytes;
    s_tex_total += bytes;
    return r;
}

static HRESULT WINAPI Dev_CreateTexture(IDirect3DDevice9 *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format,
                                        D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared) {
    auto create = orig<CreateTexture_t>(VT_DEV, dev, D_CREATETEXTURE);
    if (pool != D3DPOOL_MANAGED || !s_device_ex) return create(dev, w, h, levels, usage, format, pool, out, shared);
    HRESULT hr = create(dev, w, h, levels, usage, format, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) {
        if (++s_create_fail <= 10) log("CreateTexture %ux%u format %08x in video memory failed %08lx", w, h, format, hr);
        return hr;
    }
    IDirect3DTexture9 *t = *out;
    hook(VT_TEX, t, T_RELEASE, (void *)Tex_Release);
    hook(VT_TEX, t, T_GETSURFACE, (void *)Tex_GetSurfaceLevel);
    hook(VT_TEX, t, T_LOCK, (void *)Tex_LockRect);
    hook(VT_TEX, t, T_UNLOCK, (void *)Tex_UnlockRect);
    hook(VT_TEX, t, T_ADDDIRTY, (void *)Tex_AddDirtyRect);
    D3DSURFACE_DESC d0 = {};
    UINT rw = w, rh = h;
    D3DFORMAT rfmt = format;
    if (SUCCEEDED(t->GetLevelDesc(0, &d0))) { rw = d0.Width; rh = d0.Height; rfmt = d0.Format; }
    UINT nlevels = t->GetLevelCount();
    CsGuard g;
    if (find_res(t)) forget_res(t);
    Res *r = new_res(VT_TEX, dev, rfmt, nlevels, chain_bytes(rfmt, rw, rh, nlevels));
    r->w = rw;
    r->h = rh;
    if (g_settings.keep_system_copy) {
        IDirect3DTexture9 *c = nullptr;
        create(dev, w, h, levels, 0, format, D3DPOOL_SYSTEMMEM, &c, nullptr);
        r->sys_copy = c;
    }
    g_res.insert(t, r);
    return hr;
}

static HRESULT WINAPI Dev_CreateCubeTexture(IDirect3DDevice9 *dev, UINT edge, UINT levels, DWORD usage, D3DFORMAT format,
                                            D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *shared) {
    auto create = orig<CreateCubeTexture_t>(VT_DEV, dev, D_CREATECUBE);
    if (pool != D3DPOOL_MANAGED || !s_device_ex) return create(dev, edge, levels, usage, format, pool, out, shared);
    HRESULT hr = create(dev, edge, levels, usage, format, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) {
        if (++s_create_fail <= 10) log("CreateCubeTexture %u format %08x in video memory failed %08lx", edge, format, hr);
        return hr;
    }
    IDirect3DCubeTexture9 *t = *out;
    hook(VT_CUBE, t, T_RELEASE, (void *)Cube_Release);
    hook(VT_CUBE, t, T_GETSURFACE, (void *)Cube_GetSurface);
    hook(VT_CUBE, t, T_LOCK, (void *)Cube_LockRect);
    hook(VT_CUBE, t, T_UNLOCK, (void *)Cube_UnlockRect);
    hook(VT_CUBE, t, T_ADDDIRTY, (void *)Cube_AddDirtyRect);
    D3DSURFACE_DESC d0 = {};
    UINT redge = edge;
    D3DFORMAT rfmt = format;
    if (SUCCEEDED(t->GetLevelDesc(0, &d0))) { redge = d0.Width; rfmt = d0.Format; }
    UINT nlevels = t->GetLevelCount();
    CsGuard g;
    if (find_res(t)) forget_res(t);
    Res *r = new_res(VT_CUBE, dev, rfmt, nlevels, 6 * chain_bytes(rfmt, redge, redge, nlevels));
    r->w = r->h = redge;
    if (g_settings.keep_system_copy) {
        IDirect3DCubeTexture9 *c = nullptr;
        create(dev, edge, levels, 0, format, D3DPOOL_SYSTEMMEM, &c, nullptr);
        r->sys_copy = c;
    }
    g_res.insert(t, r);
    return hr;
}

static HRESULT WINAPI Dev_CreateVolumeTexture(IDirect3DDevice9 *dev, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                              D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *shared) {
    auto create = orig<CreateVolumeTexture_t>(VT_DEV, dev, D_CREATEVOLUME);
    if (pool != D3DPOOL_MANAGED || !s_device_ex) return create(dev, w, h, depth, levels, usage, format, pool, out, shared);
    // The Ex device has no managed pool at all, so this conversion is needed even with offloading off.
    IDirect3DVolumeTexture9 *shadow = nullptr;
    HRESULT hr = create(dev, w, h, depth, levels, 0, format, D3DPOOL_SYSTEMMEM, &shadow, nullptr);
    if (FAILED(hr)) return hr;
    hr = create(dev, w, h, depth, levels, usage, format, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) { shadow->Release(); return hr; }
    IDirect3DVolumeTexture9 *t = *out;
    hook(VT_VOL, t, T_RELEASE, (void *)Vol_Release);
    hook(VT_VOL, t, T_LOCK, (void *)Vol_LockBox);
    hook(VT_VOL, t, T_UNLOCK, (void *)Vol_UnlockBox);
    hook(VT_VOL, t, T_ADDDIRTY, (void *)Vol_AddDirtyBox);
    CsGuard g;
    if (find_res(t)) forget_res(t);
    Res *r = new_res(VT_VOL, dev, format, t->GetLevelCount(), 0);
    r->w = w;
    r->h = h;
    r->vol_shadow = shadow;
    g_res.insert(t, r);
    return hr;
}

static HRESULT WINAPI Dev_CreateVertexBuffer(IDirect3DDevice9 *dev, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                             IDirect3DVertexBuffer9 **out, HANDLE *shared) {
    auto create = orig<CreateVertexBuffer_t>(VT_DEV, dev, D_CREATEVB);
    if (pool != D3DPOOL_MANAGED || !s_device_ex) return create(dev, length, usage, fvf, pool, out, shared);
    if (!g_settings.offload_buffers) return create(dev, length, usage, fvf, D3DPOOL_SYSTEMMEM, out, shared);
    HRESULT hr = create(dev, length, usage, fvf, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) {
        if (++s_create_fail <= 10) log("CreateVertexBuffer %u bytes in video memory failed %08lx", length, hr);
        return hr;
    }
    hook(VT_VB, *out, B_RELEASE, (void *)Vb_Release);
    hook(VT_VB, *out, B_LOCK, (void *)Vb_Lock);
    CsGuard g;
    if (g_bufs.find(*out)) forget_buf_locked(*out);
    g_bufs.insert(*out, length);
    buf_bloom_add(*out);
    s_buf_count++;
    s_buf_bytes += length;
    s_buf_total += length;
    if (g_settings.keep_system_copy) {
        IDirect3DVertexBuffer9 *c = nullptr;
        create(dev, length, 0, fvf, D3DPOOL_SYSTEMMEM, &c, nullptr);
        g_buf_copies.insert(*out, c);
    }
    return hr;
}

static HRESULT WINAPI Dev_CreateIndexBuffer(IDirect3DDevice9 *dev, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                            IDirect3DIndexBuffer9 **out, HANDLE *shared) {
    auto create = orig<CreateIndexBuffer_t>(VT_DEV, dev, D_CREATEIB);
    if (pool != D3DPOOL_MANAGED || !s_device_ex) return create(dev, length, usage, format, pool, out, shared);
    if (!g_settings.offload_buffers) return create(dev, length, usage, format, D3DPOOL_SYSTEMMEM, out, shared);
    HRESULT hr = create(dev, length, usage, format, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) {
        if (++s_create_fail <= 10) log("CreateIndexBuffer %u bytes in video memory failed %08lx", length, hr);
        return hr;
    }
    hook(VT_IB, *out, B_RELEASE, (void *)Ib_Release);
    hook(VT_IB, *out, B_LOCK, (void *)Ib_Lock);
    CsGuard g;
    if (g_bufs.find(*out)) forget_buf_locked(*out);
    g_bufs.insert(*out, length);
    buf_bloom_add(*out);
    s_buf_count++;
    s_buf_bytes += length;
    s_buf_total += length;
    if (g_settings.keep_system_copy) {
        IDirect3DIndexBuffer9 *c = nullptr;
        create(dev, length, 0, format, D3DPOOL_SYSTEMMEM, &c, nullptr);
        g_buf_copies.insert(*out, c);
    }
    return hr;
}

// A 9Ex device is never "lost". When a fullscreen game comes back after Alt+Tab and the display
// mode changed, Present returns S_PRESENT_MODE_CHANGED and the device must be Reset. The game only
// knows the old protocol, so it is told the device was lost (Present -> D3DERR_DEVICELOST,
// TestCooperativeLevel -> D3DERR_DEVICENOTRESET) and then runs its own reset path.
typedef HRESULT(WINAPI *TestCoop_t)(IDirect3DDevice9 *);
enum { D_TESTCOOP = 3 };
static std::atomic<long> s_present_codes{0};
static std::atomic<bool> s_need_reset{false};

static HRESULT WINAPI Dev_TestCooperativeLevel(IDirect3DDevice9 *dev) {
    if (s_need_reset.load(std::memory_order_relaxed)) return D3DERR_DEVICENOTRESET;
    return orig<TestCoop_t>(VT_DEV, dev, D_TESTCOOP)(dev);
}

static void hook_device(IDirect3DDevice9 *dev);
static UINT s_adapter = D3DADAPTER_DEFAULT;

static D3DFORMAT strip_alpha_format(D3DFORMAT fmt) {
    if (fmt == D3DFMT_A8R8G8B8) return D3DFMT_X8R8G8B8;
    if (fmt == D3DFMT_A1R5G5B5) return D3DFMT_X1R5G5B5;
    return fmt;
}

static D3DDISPLAYMODEEX build_fullscreen_mode(const D3DPRESENT_PARAMETERS *pp, IDirect3D9 *d3d, UINT adapter) {
    D3DDISPLAYMODEEX mode = {};
    mode.Size = sizeof(mode);
    mode.Width = pp->BackBufferWidth;
    mode.Height = pp->BackBufferHeight;
    mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
    mode.Format = pp->BackBufferFormat;
    if (mode.Format == D3DFMT_UNKNOWN) {
        D3DDISPLAYMODE cur = {};
        mode.Format = (d3d && SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &cur))) ? cur.Format : D3DFMT_X8R8G8B8;
    }
    mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    return mode;
}

static HRESULT WINAPI Dev_Present(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *d) {
    if (s_device_ex.load(std::memory_order_relaxed) && (*(void ***)dev)[D_CREATETEXTURE] != (void *)Dev_CreateTexture)
        hook_device(dev);
    if (s_need_reset.load(std::memory_order_relaxed)) return D3DERR_DEVICELOST;
    if (g_dirty_count.load(std::memory_order_acquire) > 0) {
        // changes to textures not drawn with this frame: upload them now, at most once per frame
        CsGuard g;
        for (size_t i = g_dirty.size(); i-- > 0;) {
            void *t = g_dirty[i];
            if (Res *r = find_res(t))
                if (r->dev == dev && r->dirty_mask) flush_shadow(r, t);
        }
        if (g_dirty.empty()) {
            g_bound_dirty_mask.store(0, std::memory_order_release);
        } else {
            uint32_t bound_still_dirty = 0;
            for (int i = 0; i < 20; ++i) {
                void *bt = g_bound[i].load(std::memory_order_relaxed);
                if (!bt) continue;
                if (Res *r = find_res(bt))
                    if (r->dirty_mask & ~r->open_mask) bound_still_dirty |= 1u << i;
            }
            g_bound_dirty_mask.store(bound_still_dirty, std::memory_order_release);
        }
    }
    HRESULT hr = orig<Present_t>(VT_DEV, dev, D_PRESENT)(dev, a, b, w, d);
    if (hr == D3D_OK || hr == S_PRESENT_OCCLUDED) return hr;
    if (++s_present_codes <= 20) log("Present returned %08lx", hr);
    if (hr == S_PRESENT_MODE_CHANGED) {
        s_need_reset.store(true, std::memory_order_relaxed);
        return D3DERR_DEVICELOST;
    }
    return hr;
}

// SetTexture: a rewritten texture is uploaded when it is bound; draws upload bound textures changed since.
typedef HRESULT(WINAPI *SetTexture_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT(WINAPI *DrawPrimitive_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(WINAPI *DrawIndexedPrimitive_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI *DrawPrimitiveUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT(WINAPI *DrawIndexedPrimitiveUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
enum { D_SETTEXTURE = 65, D_DRAWPRIM = 81, D_DRAWINDEXED = 82, D_DRAWPRIMUP = 83, D_DRAWINDEXEDUP = 84 };

static HRESULT WINAPI Dev_SetTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *t) {
    int slot = sampler_slot(stage);
    if (slot >= 0) g_bound[slot].store(t, std::memory_order_relaxed);
    if (t && g_dirty_count.load(std::memory_order_acquire) > 0 && dirty_bloom_maybe(t)) {
        CsGuard g;
        if (Res *r = find_res(t))
            if (r->dirty_mask) flush_shadow(r, t);
    }
    return orig<SetTexture_t>(VT_DEV, dev, D_SETTEXTURE)(dev, stage, t);
}
// StretchRect can copy from a level of a rewritten texture: its changes go up first
static HRESULT WINAPI Dev_StretchRect(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst,
                                      const RECT *dr, D3DTEXTUREFILTERTYPE filter) {
    if (src && g_dirty_count.load(std::memory_order_acquire) > 0 && s_surf_count.load(std::memory_order_relaxed) > 0) {
        CsGuard g;
        if (SurfRef *ref = g_surfs.find(src))
            if (Res *r = find_res(ref->tex))
                if (r->dirty_mask) flush_shadow(r, ref->tex);
    }
    return orig<StretchRect_t>(VT_DEV, dev, D_STRETCHRECT)(dev, src, sr, dst, dr, filter);
}
static __forceinline void before_draw() {
    if (g_bound_dirty_mask.load(std::memory_order_acquire) != 0) { CsGuard g; flush_bound(); }
}
static HRESULT WINAPI Dev_DrawPrimitive(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    before_draw();
    return orig<DrawPrimitive_t>(VT_DEV, dev, D_DRAWPRIM)(dev, type, start, count);
}
static HRESULT WINAPI Dev_DrawIndexedPrimitive(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, INT base, UINT min, UINT num, UINT start, UINT count) {
    before_draw();
    return orig<DrawIndexedPrimitive_t>(VT_DEV, dev, D_DRAWINDEXED)(dev, type, base, min, num, start, count);
}
static HRESULT WINAPI Dev_DrawPrimitiveUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride) {
    before_draw();
    return orig<DrawPrimitiveUP_t>(VT_DEV, dev, D_DRAWPRIMUP)(dev, type, count, data, stride);
}
static HRESULT WINAPI Dev_DrawIndexedPrimitiveUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT min, UINT num, UINT count,
                                                 const void *idx, D3DFORMAT fmt, const void *data, UINT stride) {
    before_draw();
    return orig<DrawIndexedPrimitiveUP_t>(VT_DEV, dev, D_DRAWINDEXEDUP)(dev, type, min, num, count, idx, fmt, data, stride);
}

typedef HRESULT(WINAPI *BeginStateBlock_t)(IDirect3DDevice9 *);
typedef HRESULT(WINAPI *EndStateBlock_t)(IDirect3DDevice9 *, IDirect3DStateBlock9 **);
enum { D_BEGINSTATEBLOCK = 60, D_ENDSTATEBLOCK = 61 };

static HRESULT WINAPI Dev_BeginStateBlock(IDirect3DDevice9 *dev) {
    HRESULT hr = orig<BeginStateBlock_t>(VT_DEV, dev, D_BEGINSTATEBLOCK)(dev);
    // Native d3d9 swaps the device vtable during state-block recording; re-apply hooks on the new table.
    if (SUCCEEDED(hr) && s_device_ex.load(std::memory_order_relaxed)) hook_device(dev);
    return hr;
}

static HRESULT WINAPI Dev_EndStateBlock(IDirect3DDevice9 *dev, IDirect3DStateBlock9 **sb) {
    HRESULT hr = orig<EndStateBlock_t>(VT_DEV, dev, D_ENDSTATEBLOCK)(dev, sb);
    // Native d3d9 restores the normal device vtable with unhooked pointers; re-apply hooks immediately.
    if (s_device_ex.load(std::memory_order_relaxed)) hook_device(dev);
    return hr;
}

static HRESULT WINAPI Dev_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
    HRESULT hr = orig<Reset_t>(VT_DEV, dev, D_RESET)(dev, pp);
    bool used_ex = false;
    if (FAILED(hr) && s_device_ex.load(std::memory_order_acquire) && pp && !pp->Windowed) {
        IDirect3DDevice9Ex *dev_ex = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&dev_ex)) && dev_ex) {
            IDirect3D9 *d3d = nullptr;
            if (pp->BackBufferFormat == D3DFMT_UNKNOWN) dev->GetDirect3D(&d3d);
            D3DDISPLAYMODEEX mode = build_fullscreen_mode(pp, d3d, s_adapter);
            if (d3d) d3d->Release();
            hr = dev_ex->ResetEx(pp, &mode);
            if (FAILED(hr) && strip_alpha_format(pp->BackBufferFormat) != pp->BackBufferFormat) {
                D3DPRESENT_PARAMETERS pp_noa = *pp;
                pp_noa.BackBufferFormat = strip_alpha_format(pp->BackBufferFormat);
                mode.Format = pp_noa.BackBufferFormat;
                HRESULT hr2 = dev_ex->ResetEx(&pp_noa, &mode);
                if (SUCCEEDED(hr2)) { *pp = pp_noa; hr = hr2; }
            }
            dev_ex->Release();
            used_ex = true;
        }
    }
    if (SUCCEEDED(hr)) {
        if (s_device_ex.load(std::memory_order_relaxed)) hook_device(dev);
        s_need_reset.store(false, std::memory_order_relaxed);
        CsGuard g;
        for (auto &b : g_bound) b.store(nullptr, std::memory_order_relaxed);
        g_bound_dirty_mask.store(0, std::memory_order_release);
    }
    log("Reset%s %ux%u windowed=%d fmt=%u: %08lx", used_ex ? "Ex" : "", pp ? pp->BackBufferWidth : 0,
        pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1, pp ? (unsigned)pp->BackBufferFormat : 0u, hr);
    return hr;
}

static ULONG WINAPI Dev_Release(IDirect3DDevice9 *dev) {
    ULONG n = orig<Release_t>(VT_DEV, dev, D_RELEASE)(dev);
    if (g_in_dev_release) return n;
    std::vector<IDirect3DTexture9 *> drop;
    {
        CsGuard g;
        ULONG s_cnt = 0;
        for (const auto &it : g_staging_cache)
            if (it.key.dev == dev) ++s_cnt;
        // Native d3d9 staging textures hold a COM reference on dev (so n == s_cnt when the last app ref drops);
        // wrappers that use internal refs report n == 0. Only tear down when the device is actually dying.
        bool dying = (g_staging_adds_dev_ref == 0) ? (n == 0) : (n <= s_cnt);
        if (dying) {
            size_t keep = 0;
            for (size_t i = 0; i < g_staging_cache.size(); ++i) {
                if (g_staging_cache[i].key.dev == dev) {
                    g_staging_bytes -= g_staging_cache[i].bytes;
                    drop.push_back(g_staging_cache[i].tex);
                } else {
                    g_staging_cache[keep++] = g_staging_cache[i];
                }
            }
            g_staging_cache.resize(keep);
            if (g_active_dev == dev) {
                g_active_dev = nullptr;
                for (auto &b : g_bound) b.store(nullptr, std::memory_order_relaxed);
                g_bound_dirty_mask.store(0, std::memory_order_release);
                s_need_reset.store(false, std::memory_order_relaxed);
            }
            n = 0;
        }
    }
    if (!drop.empty()) {
        g_in_dev_release = true;
        for (IDirect3DTexture9 *t : drop) t->Release();
        g_in_dev_release = false;
    }
    return n;
}

static void hook_device(IDirect3DDevice9 *dev) {
    hook(VT_DEV, dev, D_RELEASE, (void *)Dev_Release);
    hook(VT_DEV, dev, D_CREATETEXTURE, (void *)Dev_CreateTexture);
    hook(VT_DEV, dev, D_CREATECUBE, (void *)Dev_CreateCubeTexture);
    hook(VT_DEV, dev, D_CREATEVOLUME, (void *)Dev_CreateVolumeTexture);
    hook(VT_DEV, dev, D_CREATEVB, (void *)Dev_CreateVertexBuffer);
    hook(VT_DEV, dev, D_CREATEIB, (void *)Dev_CreateIndexBuffer);
    hook(VT_DEV, dev, D_PRESENT, (void *)Dev_Present);
    hook(VT_DEV, dev, D_RESET, (void *)Dev_Reset);
    hook(VT_DEV, dev, D_TESTCOOP, (void *)Dev_TestCooperativeLevel);
    hook(VT_DEV, dev, D_BEGINSTATEBLOCK, (void *)Dev_BeginStateBlock);
    hook(VT_DEV, dev, D_ENDSTATEBLOCK, (void *)Dev_EndStateBlock);
    hook(VT_DEV, dev, D_SETTEXTURE, (void *)Dev_SetTexture);
    hook(VT_DEV, dev, D_STRETCHRECT, (void *)Dev_StretchRect);
    hook(VT_DEV, dev, D_DRAWPRIM, (void *)Dev_DrawPrimitive);
    hook(VT_DEV, dev, D_DRAWINDEXED, (void *)Dev_DrawIndexedPrimitive);
    hook(VT_DEV, dev, D_DRAWPRIMUP, (void *)Dev_DrawPrimitiveUP);
    hook(VT_DEV, dev, D_DRAWINDEXEDUP, (void *)Dev_DrawIndexedPrimitiveUP);
    // Make sure the originals used internally exist even if never hooked through a call.
    save_orig(VT_DEV, dev, D_UPDATESURFACE);
    save_orig(VT_DEV, dev, D_UPDATETEXTURE);
}

// ---------------------------------------------------------------- Direct3DCreate9 export hook
typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT(WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex **);
static Direct3DCreate9_t g_create9;
static Direct3DCreate9Ex_t g_create9ex;

// ---------------------------------------------------------------- IDirect3D9 hooks
static HRESULT WINAPI D3D_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                       D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    auto create = orig<CreateDevice_t>(VT_D3D, d3d, 16);
    IDirect3D9Ex *ex = nullptr;
    if (g_settings.d3d9ex && pp && out && SUCCEEDED(d3d->QueryInterface(__uuidof(IDirect3D9Ex), (void **)&ex))) {
        D3DPRESENT_PARAMETERS pp_ex = *pp;
        D3DDISPLAYMODEEX mode = build_fullscreen_mode(&pp_ex, d3d, adapter);
        IDirect3DDevice9Ex *dev = nullptr;
        HRESULT hr = ex->CreateDeviceEx(adapter, type, focus, flags, &pp_ex, pp_ex.Windowed ? nullptr : &mode, &dev);
        // On some drivers an alpha fullscreen backbuffer (e.g. A8R8G8B8) is rejected by CreateDeviceEx;
        // retry once with the matching non-alpha display format on both pp and mode (D3D9Ex requires them to match).
        if (FAILED(hr) && !pp->Windowed && strip_alpha_format(pp->BackBufferFormat) != pp->BackBufferFormat) {
            pp_ex = *pp;
            pp_ex.BackBufferFormat = strip_alpha_format(pp->BackBufferFormat);
            mode = build_fullscreen_mode(&pp_ex, d3d, adapter);
            hr = ex->CreateDeviceEx(adapter, type, focus, flags, &pp_ex, &mode, &dev);
        }
        ex->Release();
        D3DADAPTER_IDENTIFIER9 id = {};
        d3d->GetAdapterIdentifier(adapter, 0, &id);
        if (SUCCEEDED(hr)) {
            *pp = pp_ex;
            s_adapter = adapter;
            s_device_ex.store(true, std::memory_order_release);
            s_multithreaded.store((flags & D3DCREATE_MULTITHREADED) != 0, std::memory_order_release);
            s_need_reset.store(false, std::memory_order_relaxed);
            { CsGuard g; g_active_dev = dev; g_staging_adds_dev_ref = -1; }
            *out = dev;
            hook_device(dev);
            log("device: Direct3D 9Ex on '%s', %ux%u %s (fmt %u, mode fmt %u), flags %08lx; offloading textures=%d buffers=%d",
                id.Description, pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed ? "windowed" : "fullscreen",
                (unsigned)pp->BackBufferFormat, (unsigned)mode.Format, flags, g_settings.offload_textures,
                g_settings.offload_buffers);
            return hr;
        }
        log("device: CreateDeviceEx on '%s' failed %08lx (windowed=%d fmt=%u mode_fmt=%u); falling back to normal Direct3D 9",
            id.Description, hr, pp->Windowed, (unsigned)pp->BackBufferFormat, (unsigned)mode.Format);
        // Calling CreateDevice on an IDirect3D9Ex object still returns an IDirect3DDevice9Ex where
        // D3DPOOL_MANAGED fails with D3DERR_INVALIDCALL. Fall back through a genuine non-Ex IDirect3D9.
        if (g_create9) {
            if (IDirect3D9 *plain = g_create9(D3D_SDK_VERSION)) {
                HRESULT fb = plain->CreateDevice(adapter, type, focus, flags, pp, out);
                plain->Release();
                if (SUCCEEDED(fb)) {
                    s_adapter = adapter;
                    s_device_ex.store(false, std::memory_order_release);
                    s_multithreaded.store((flags & D3DCREATE_MULTITHREADED) != 0, std::memory_order_release);
                    s_need_reset.store(false, std::memory_order_relaxed);
                    log("device: normal Direct3D 9 device via fallback (nothing offloaded)");
                    return fb;
                }
                log("device: fallback CreateDevice on normal Direct3D 9 failed %08lx", fb);
            }
        }
    }
    HRESULT hr = create(d3d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        IDirect3DDevice9Ex *check_ex = nullptr;
        bool is_ex = SUCCEEDED((*out)->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&check_ex)) && check_ex;
        if (check_ex) check_ex->Release();
        s_adapter = adapter;
        s_device_ex.store(is_ex, std::memory_order_release);
        s_multithreaded.store((flags & D3DCREATE_MULTITHREADED) != 0, std::memory_order_release);
        s_need_reset.store(false, std::memory_order_relaxed);
        if (is_ex) {
            { CsGuard g; g_active_dev = *out; g_staging_adds_dev_ref = -1; }
            hook_device(*out);
            log("device: CreateDevice returned an IDirect3DDevice9Ex; hooks enabled");
        } else {
            log("device: normal Direct3D 9 device (nothing offloaded)");
        }
    }
    return hr;
}

static IDirect3D9 *WINAPI my_Direct3DCreate9(UINT sdk) {
    if (g_settings.d3d9ex && g_create9ex) {
        IDirect3D9Ex *ex = nullptr;
        HRESULT hr = g_create9ex(sdk, &ex);
        if (SUCCEEDED(hr) && ex) {
            hook(VT_D3D, ex, 16, (void *)D3D_CreateDevice);
            log("Direct3DCreate9(%u): gave out an IDirect3D9Ex", sdk);
            return ex;
        }
        log("Direct3DCreate9Ex failed %08lx; normal Direct3D 9", hr);
    }
    return g_create9(sdk);
}

bool patch_export(HMODULE mod, const char *name, void *fn, void **old) {
    if (!mod || !name || !fn) return false;
    __try {
        BYTE *base = (BYTE *)mod;
        auto dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        if (dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return false;
        auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        DWORD img_size = nt->OptionalHeader.SizeOfImage;
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= img_size) return false;
        auto exp = (IMAGE_EXPORT_DIRECTORY *)(base + dir.VirtualAddress);
        if (exp->AddressOfFunctions >= img_size || exp->AddressOfNames >= img_size || exp->AddressOfNameOrdinals >= img_size) return false;
        DWORD *funcs = (DWORD *)(base + exp->AddressOfFunctions);
        DWORD *names = (DWORD *)(base + exp->AddressOfNames);
        WORD *ords = (WORD *)(base + exp->AddressOfNameOrdinals);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            if (names[i] >= img_size || ords[i] >= exp->NumberOfFunctions) continue;
            if (strcmp((char *)(base + names[i]), name) != 0) continue;
            DWORD *slot = &funcs[ords[i]];
            DWORD cur_rva = *slot;
            if (cur_rva >= dir.VirtualAddress && cur_rva < dir.VirtualAddress + dir.Size) return false;   // existing forwarder
            DWORD rva = (DWORD)((uintptr_t)fn - (uintptr_t)base);   // wraps modulo 2^32, as the loader adds it
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return false;   // would read as a forwarder
            DWORD prot;
            if (!VirtualProtect(slot, 4, PAGE_READWRITE, &prot)) return false;
            if (old) *old = base + cur_rva;
            InterlockedExchange((volatile LONG *)slot, (LONG)rva);
            VirtualProtect(slot, 4, prot, &prot);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static int patch_imports_module(BYTE *base, void *old_fn, void *new_fn) {
    int count = 0;
    __try {
        auto dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        if (dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return 0;
        auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        DWORD img_size = nt->OptionalHeader.SizeOfImage;
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || dir.VirtualAddress >= img_size) return 0;
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
             (DWORD)((BYTE *)imp - base) + sizeof(*imp) <= img_size && imp->Name; ++imp) {
            if (!imp->FirstThunk || imp->Name >= img_size || imp->FirstThunk >= img_size) continue;
            if (_stricmp((char *)(base + imp->Name), "d3d9.dll") != 0) continue;
            for (auto th = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
                 (DWORD)((BYTE *)th - base) + sizeof(*th) <= img_size && th->u1.Function; ++th) {
                if ((void *)th->u1.Function != old_fn) continue;
                DWORD prot;
                if (VirtualProtect(&th->u1.Function, 4, PAGE_READWRITE, &prot)) {
                    InterlockedExchange((volatile LONG *)&th->u1.Function, (LONG)(uintptr_t)new_fn);
                    VirtualProtect(&th->u1.Function, 4, prot, &prot);
                    ++count;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// Modules loaded before us that already imported the function get their import slot changed too.
static int patch_imports(void *old_fn, void *new_fn) {
    int count = 0;
    HMODULE mods[512];
    DWORD needed = 0;
    typedef BOOL(WINAPI * EnumMods_t)(HANDLE, HMODULE *, DWORD, LPDWORD);
    auto enum_mods = (EnumMods_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    if (!enum_mods || !enum_mods(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    for (DWORD m = 0; m < needed / sizeof(HMODULE) && m < 512; ++m) {
        if (mods[m] == (HMODULE)&__ImageBase) continue;
        count += patch_imports_module((BYTE *)mods[m], old_fn, new_fn);
    }
    return count;
}

void install_d3d9() {
    InitializeCriticalSectionAndSpinCount(&g_cs, 4000);
    // The server process (process split) needs this too: shaderapiempty.dll reads the hardware caps
    // through shaderapidx9.dll's Direct3DCreate9, which fails on a locked or non-interactive desktop
    // while Direct3DCreate9Ex works. It creates no device, so nothing is offloaded there.
    if (!g_settings.d3d9ex) { log("d3d9: switched off in the settings"); return; }
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    if (!d3d9) { log("d3d9: d3d9.dll could not be loaded (%lu)", GetLastError()); return; }
    wchar_t path[MAX_PATH], sys[MAX_PATH], wow[MAX_PATH];
    if (!GetModuleFileNameW(d3d9, path, MAX_PATH)) path[0] = 0;
    // A 32-bit process on 64-bit Windows sees System32 in module paths (redirected to SysWOW64).
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    UINT m = GetSystemWow64DirectoryW(wow, MAX_PATH);
    bool system = (n > 0 && n < MAX_PATH && _wcsnicmp(path, sys, n) == 0 && path[n] == L'\\') ||
                  (m > 0 && m < MAX_PATH && _wcsnicmp(path, wow, m) == 0 && path[m] == L'\\');
    if (!system) {
        if (!g_settings.allow_wrapper) {
            log("d3d9: %ls is not Windows' own d3d9.dll (DXVK, ReShade or another wrapper); the D3D9 part stays off "
                "(set allow_d3d9_wrapper=1 to try it anyway)", path);
            return;
        }
        log("d3d9: using the wrapper %ls (allow_d3d9_wrapper=1)", path);
    }
    g_create9ex = (Direct3DCreate9Ex_t)GetProcAddress(d3d9, "Direct3DCreate9Ex");
    if (!g_create9ex) { log("d3d9: no Direct3DCreate9Ex in %ls; the D3D9 part stays off", path); return; }
    void *old = nullptr;
    if (!patch_export(d3d9, "Direct3DCreate9", (void *)my_Direct3DCreate9, &old)) { log("d3d9: could not hook Direct3DCreate9"); return; }
    g_create9 = (Direct3DCreate9_t)old;
    int imports = patch_imports(old, (void *)my_Direct3DCreate9);
    log("d3d9: hooked Direct3DCreate9 in %ls (%d existing import(s) changed)", path, imports);
}

// ---------------------------------------------------------------- periodic statistics
// The engine's hunk (map data, with the sound/model cache in its free middle). engine.dll 1.2 at
// its preferred base: size at 0x213066F0, low mark 0x213066EC, high mark 0x213066F4.
static bool engine_hunk(unsigned *size_mb, unsigned *low_mb, unsigned *high_mb, unsigned *free_mb) {
    HMODULE engine = GetModuleHandleW(L"engine.dll");
    if (engine != (HMODULE)0x20000000) return false;
    __try {
        // the code that sets them (Memory_Init) must be the analysed one
        // mov eax,[20B42BC4]; mov ecx,[20B42BC8]; sub esp,14h; push ebx,ebp,esi,edi; mov [21306698],eax
        static const BYTE expect[] = {0xA1, 0xC4, 0x2B, 0xB4, 0x20, 0x8B, 0x0D, 0xC8, 0x2B, 0xB4, 0x20, 0x83, 0xEC, 0x14,
                                      0x53, 0x55, 0x56, 0x57, 0xA3, 0x98, 0x66, 0x30, 0x21};
        if (memcmp((const void *)0x20114990, expect, sizeof(expect)) != 0) return false;
        unsigned size = *(unsigned *)0x213066F0, low = *(unsigned *)0x213066EC, high = *(unsigned *)0x213066F4;
        if (size < (16u << 20) || size > (1800u << 20) || low > size || high > size || low > size - high) return false;
        *size_mb = size >> 20; *low_mb = low >> 20; *high_mb = high >> 20; *free_mb = (size - low - high) >> 20;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static DWORD WINAPI stats_thread(void *) {
    for (;;) {
        Sleep((DWORD)g_settings.stats_seconds * 1000u);
        VaStats va = va_stats();
        long long staging, shadow;
        long shadows, coalesced, flushes;
        {
            CsGuard g;
            staging = g_staging_bytes;
            shadow = s_shadow_bytes;
            shadows = s_shadow_count;
            coalesced = s_coalesced;
            flushes = s_flushes;
        }
        log("stats: address space %llu MB used, %llu MB free, largest block %llu MB | in video memory instead of "
            "the address space: %lld textures %lld MB, %lld buffers %lld MB (created so far %lld + %lld MB) | locks "
            "full %ld partial %ld read %ld failed %ld/%ld | upload cache %lld KB | rewritten textures %ld (%lld KB), "
            "%ld uploads, %ld changes merged",
            va.used >> 20, va.free_total >> 20, va.largest_free >> 20, (long long)s_tex_count, (long long)s_tex_bytes >> 20,
            (long long)s_buf_count, (long long)s_buf_bytes >> 20, (long long)s_tex_total >> 20, (long long)s_buf_total >> 20,
            (long)s_lock_full, (long)s_lock_partial, (long)s_lock_read, (long)s_lock_fail, (long)s_upload_fail, staging >> 10,
            shadows, shadow >> 10, flushes, coalesced);
        if (g_settings.verbose) {
            struct TopItem { VtKind kind; UINT w, h, levels; D3DFORMAT format; long uploads; };
            TopItem top[5] = {};
            int ntop = 0;
            {
                CsGuard g;
                Res *best[5] = {};
                g_res.for_each([&](void *, Res *rv) {
                    for (int i = 0; i < 5; ++i)
                        if (!best[i] || rv->upload_count > best[i]->upload_count) {
                            for (int j = 4; j > i; --j) best[j] = best[j - 1];
                            best[i] = rv;
                            break;
                        }
                });
                for (int i = 0; i < 5 && best[i]; ++i)
                    top[ntop++] = {best[i]->kind, best[i]->w, best[i]->h, best[i]->levels, best[i]->format, best[i]->upload_count};
            }
            for (int i = 0; i < ntop; ++i)
                log("stats: most uploaded #%d: %s %ux%u format %08x levels %u, %ld locks", i + 1,
                    top[i].kind == VT_CUBE ? "cube" : top[i].kind == VT_VOL ? "volume" : "texture", top[i].w, top[i].h,
                    top[i].format, top[i].levels, top[i].uploads);
        }
        unsigned hs, hl, hh, hf;
        if (engine_hunk(&hs, &hl, &hh, &hf)) log("stats: engine hunk %u MB: %u MB map data (low), %u MB high, %u MB free for the cache", hs, hl, hh, hf);
    }
}

// Exported for tests and other mods: current figures.
extern "C" __declspec(dllexport) void __cdecl MemoryExpansion_GetStats(long long out[8]) {
    out[0] = s_tex_count;
    out[1] = s_tex_bytes;
    out[2] = s_buf_count;
    out[3] = s_buf_bytes;
    out[4] = s_lock_full + s_lock_partial;
    out[5] = s_lock_fail + s_upload_fail + s_create_fail;
    out[6] = s_device_ex ? 1 : 0;
    CsGuard g;
    out[7] = (long long)g_surfs.size();
}

// test only: rewritten textures (copies made, uploads, changes merged)
extern "C" __declspec(dllexport) void __cdecl MemoryExpansion_GetRewriteStats(long long out[3]) {
    CsGuard g;
    out[0] = s_shadow_count;
    out[1] = s_flushes;
    out[2] = s_coalesced;
}

void start_stats() {
    if (g_settings.stats_seconds <= 0) return;
    HANDLE t = CreateThread(nullptr, 0, stats_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

} // namespace memx
