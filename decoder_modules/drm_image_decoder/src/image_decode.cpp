/*
 * image_decode.cpp — décode un buffer image (bytes) en pixels RGBA8.
 *
 * Supporte :
 *   - JPEG, PNG, BMP via stb_image (header-only)
 *   - JPEG 2000 (.jp2) via OpenJPEG  (libopenjp2)
 *
 * Renvoie outRGBA dimensionné width*height*4, ou false si échec.
 */

#include "image_decode.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <stb/stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb/stb_image_write.h>

#include <openjpeg-2.5/openjpeg.h>

#include <cstring>
#include <cstdio>

namespace drm {

// ---------------------------------------------------------------------------
// Détection de format (magic bytes).
// ---------------------------------------------------------------------------
const char* sniffFormat(const uint8_t* d, int n) {
    if (n < 12) return "?";
    if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
    if (d[0] == 0x89 && d[1] == 'P'  && d[2] == 'N' && d[3] == 'G') return "png";
    if (d[0] == 'B'  && d[1] == 'M')  return "bmp";
    if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0x0C &&
        d[4] == 'j' && d[5] == 'P') return "jp2";
    return "?";
}

// ---------------------------------------------------------------------------
// JP2 -> RGBA via OpenJPEG. Utilise un stream "mémoire" custom.
// ---------------------------------------------------------------------------
struct JP2MemCtx {
    const uint8_t* data;
    OPJ_SIZE_T     size;
    OPJ_SIZE_T     pos;
};
static OPJ_SIZE_T jp2_read(void* buf, OPJ_SIZE_T n, void* ud) {
    JP2MemCtx* c = (JP2MemCtx*)ud;
    if (c->pos >= c->size) return (OPJ_SIZE_T)-1;
    OPJ_SIZE_T avail = c->size - c->pos;
    OPJ_SIZE_T cnt   = n < avail ? n : avail;
    std::memcpy(buf, c->data + c->pos, cnt);
    c->pos += cnt;
    return cnt;
}
static OPJ_OFF_T jp2_skip(OPJ_OFF_T n, void* ud) {
    JP2MemCtx* c = (JP2MemCtx*)ud;
    OPJ_SIZE_T newPos = c->pos + (OPJ_SIZE_T)n;
    if (newPos > c->size) newPos = c->size;
    OPJ_OFF_T delta = (OPJ_OFF_T)(newPos - c->pos);
    c->pos = newPos;
    return delta;
}
static OPJ_BOOL jp2_seek(OPJ_OFF_T n, void* ud) {
    JP2MemCtx* c = (JP2MemCtx*)ud;
    if ((OPJ_SIZE_T)n > c->size) return OPJ_FALSE;
    c->pos = (OPJ_SIZE_T)n;
    return OPJ_TRUE;
}
static void jp2_silent(const char*, void*) {}

static bool decodeJP2(const uint8_t* data, int n,
                      std::vector<uint8_t>& outRGBA, int& width, int& height) {
    JP2MemCtx ctx { data, (OPJ_SIZE_T)n, 0 };

    opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE);
    if (!stream) return false;
    opj_stream_set_read_function(stream, jp2_read);
    opj_stream_set_skip_function(stream, jp2_skip);
    opj_stream_set_seek_function(stream, jp2_seek);
    opj_stream_set_user_data(stream, &ctx, nullptr);
    opj_stream_set_user_data_length(stream, ctx.size);

    opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!codec) { opj_stream_destroy(stream); return false; }
    opj_set_info_handler(codec, jp2_silent, nullptr);
    opj_set_warning_handler(codec, jp2_silent, nullptr);
    opj_set_error_handler(codec, jp2_silent, nullptr);

    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    if (!opj_setup_decoder(codec, &params)) {
        opj_destroy_codec(codec); opj_stream_destroy(stream); return false;
    }

    opj_image_t* image = nullptr;
    if (!opj_read_header(stream, codec, &image)) {
        opj_destroy_codec(codec); opj_stream_destroy(stream); return false;
    }
    if (!opj_decode(codec, stream, image)) {
        opj_image_destroy(image); opj_destroy_codec(codec); opj_stream_destroy(stream); return false;
    }
    if (!opj_end_decompress(codec, stream)) {
        // ce n'est pas fatal pour beaucoup de fichiers ; on continue
    }

    width  = (int)image->x1 - (int)image->x0;
    height = (int)image->y1 - (int)image->y0;
    if (width <= 0 || height <= 0 || image->numcomps < 1) {
        opj_image_destroy(image); opj_destroy_codec(codec); opj_stream_destroy(stream); return false;
    }

    outRGBA.assign((size_t)width * height * 4, 0);

    // Récupérer R/G/B (ou luminance en N&B).
    OPJ_INT32* R = image->comps[0].data;
    OPJ_INT32* G = image->numcomps >= 3 ? image->comps[1].data : R;
    OPJ_INT32* B = image->numcomps >= 3 ? image->comps[2].data : R;
    int prec  = image->comps[0].prec;
    int shift = std::max(0, prec - 8);

    bool sgnd = image->comps[0].sgnd;
    int  bias = sgnd ? (1 << (prec - 1)) : 0;

    int npx = width * height;
    for (int i = 0; i < npx; ++i) {
        int r = (R[i] + bias) >> shift; if (r < 0) r = 0; if (r > 255) r = 255;
        int g = (G[i] + bias) >> shift; if (g < 0) g = 0; if (g > 255) g = 255;
        int b = (B[i] + bias) >> shift; if (b < 0) b = 0; if (b > 255) b = 255;
        outRGBA[(size_t)i * 4 + 0] = (uint8_t)r;
        outRGBA[(size_t)i * 4 + 1] = (uint8_t)g;
        outRGBA[(size_t)i * 4 + 2] = (uint8_t)b;
        outRGBA[(size_t)i * 4 + 3] = 255;
    }

    opj_image_destroy(image);
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return true;
}

// ---------------------------------------------------------------------------
// JPEG / PNG / BMP -> RGBA via stb_image.
// ---------------------------------------------------------------------------
static bool decodeStb(const uint8_t* data, int n,
                      std::vector<uint8_t>& outRGBA, int& width, int& height) {
    int comp;
    unsigned char* px = stbi_load_from_memory(data, n, &width, &height, &comp, 4);
    if (!px) return false;
    outRGBA.assign(px, px + (size_t)width * height * 4);
    stbi_image_free(px);
    return true;
}

// ---------------------------------------------------------------------------
// Point d'entrée : auto-détection + dispatch.
// ---------------------------------------------------------------------------
bool decodeImageToRGBA(const uint8_t* data, int n,
                       std::vector<uint8_t>& outRGBA, int& width, int& height) {
    if (!data || n < 12) return false;
    const char* fmt = sniffFormat(data, n);
    if (std::strcmp(fmt, "jp2") == 0) {
        return decodeJP2(data, n, outRGBA, width, height);
    }
    if (std::strcmp(fmt, "jpeg") == 0 ||
        std::strcmp(fmt, "png")  == 0 ||
        std::strcmp(fmt, "bmp")  == 0) {
        return decodeStb(data, n, outRGBA, width, height);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Écriture : RGBA -> BMP / PNG / JPEG via stb_image_write.
// ---------------------------------------------------------------------------
bool writeRGBAToFile(const std::string& path,
                     const uint8_t* rgba, int width, int height,
                     const std::string& format, int jpegQuality) {
    if (!rgba || width <= 0 || height <= 0) return false;
    if (format == "bmp") {
        return stbi_write_bmp(path.c_str(), width, height, 4, rgba) != 0;
    }
    if (format == "png") {
        return stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4) != 0;
    }
    if (format == "jpeg" || format == "jpg") {
        // JPEG ne supporte pas l'alpha : on convertit RGBA -> RGB.
        std::vector<uint8_t> rgb((size_t)width * height * 3);
        for (int i = 0; i < width * height; ++i) {
            rgb[(size_t)i * 3 + 0] = rgba[(size_t)i * 4 + 0];
            rgb[(size_t)i * 3 + 1] = rgba[(size_t)i * 4 + 1];
            rgb[(size_t)i * 3 + 2] = rgba[(size_t)i * 4 + 2];
        }
        if (jpegQuality < 1)   jpegQuality = 1;
        if (jpegQuality > 100) jpegQuality = 100;
        return stbi_write_jpg(path.c_str(), width, height, 3, rgb.data(), jpegQuality) != 0;
    }
    return false;
}

bool writeRawBytesToFile(const std::string& path,
                         const uint8_t* data, int n) {
    if (!data || n <= 0) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t w = std::fwrite(data, 1, (size_t)n, f);
    std::fclose(f);
    return (int)w == n;
}

} // namespace drm
