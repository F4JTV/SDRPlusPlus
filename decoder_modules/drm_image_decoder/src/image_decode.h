#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace drm {

// Détecte le format d'un buffer image par ses bytes de signature.
// Retourne "jpeg", "png", "bmp", "jp2" ou "?".
const char* sniffFormat(const uint8_t* data, int n);

// Décode un buffer image (JPEG / PNG / BMP / JPEG 2000) en pixels RGBA8.
// outRGBA est redimensionné à width*height*4. Retourne false en cas d'échec.
bool decodeImageToRGBA(const uint8_t* data, int n,
                       std::vector<uint8_t>& outRGBA, int& width, int& height);

// Écrit un buffer RGBA8 (width*height*4 octets, ordre RGBA) dans un fichier.
// Format : "bmp", "png" ou "jpeg" (jpegQuality = 1..100, ignoré sinon).
// Retourne true en cas de succès.
bool writeRGBAToFile(const std::string& path,
                     const uint8_t* rgba, int width, int height,
                     const std::string& format, int jpegQuality = 90);

// Écrit le buffer image brut (tel que reçu, donc JP2/JPEG/PNG/BMP) à `path`.
bool writeRawBytesToFile(const std::string& path,
                         const uint8_t* data, int n);

} // namespace drm

