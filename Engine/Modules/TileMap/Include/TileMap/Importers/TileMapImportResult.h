#pragma once

#include "AssetCore/GUID.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace TileMap
{
namespace Importers
{

/**
 * Maps an atlas image file to the engine texture GUID a produced .tileset
 * should reference. The editor resolves through the asset registry; headless
 * callers (tests) supply a fixed mapping. A null return leaves the tileset's
 * atlas GUID null (imported, but untextured until assigned).
 */
using TextureGuidResolver = std::function<GUID(const std::filesystem::path& absoluteImagePath)>;

/** One .tileset produced by an import, under a freshly minted GUID. */
struct ImportedTileSet
{
    GUID Guid;
    std::string Name;                  // suggested asset file stem
    std::filesystem::path ImagePath;   // absolute atlas image path (may not exist)
    std::unique_ptr<TileSetAsset> Asset;
};

/** One .tilemap produced by an import; layers reference ImportedTileSet GUIDs. */
struct ImportedTileMap
{
    std::string Name; // suggested asset file stem
    std::unique_ptr<TileMapAsset> Asset;
};

/**
 * Result of a source-file import: either Success with the produced assets, or
 * a single fatal Error. Non-fatal degradations (skipped cells, unsupported
 * shapes) land in Warnings so a caller can surface them without failing.
 */
struct TileMapImportResult
{
    bool Success = false;
    std::string Error;
    std::vector<std::string> Warnings;
    std::vector<ImportedTileSet> TileSets;
    std::vector<ImportedTileMap> Maps;
};

/**
 * Rewrites every layer tileset reference `from` to `to` across the imported
 * maps — for callers whose asset registry mints the final GUID at
 * registration time rather than accepting the imported one.
 */
void RemapImportedTileSetGuid(TileMapImportResult& result, const GUID& from, const GUID& to);

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
