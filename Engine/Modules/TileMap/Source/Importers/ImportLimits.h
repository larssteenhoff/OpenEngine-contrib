#pragma once

#include "TileMap/TileSetAsset.h"

namespace GameEngine::TileMap::Importers
{

// Hard ceilings on hostile or corrupt input. Every count read from a foreign
// file is checked against one of these before any allocation sized by it, and
// counts that accumulate — cells across a layer's chunks — are checked against
// their running total, not one element at a time.
inline constexpr int32 kMaxImportCellsPerAxis = 16384;
inline constexpr int64 kMaxImportCellsPerLayer = int64(1) << 22; // 4M cells
// Cell coordinates stay well inside int32 so a chunk origin plus its extent
// cannot overflow.
inline constexpr int32 kMaxImportCellCoordinate = 1 << 24;
// A tile's width and height, and an atlas's spacing and margin, in pixels.
inline constexpr int32 kMaxImportTilePixels = 16384;
inline constexpr int32 kMaxImportLayers = 256;
inline constexpr int32 kMaxImportLayerDepth = 64;
inline constexpr int32 kMaxImportTileSets = 64;
inline constexpr int32 kMaxImportTilesPerSet = kMaxTileSetTiles;
inline constexpr int32 kMaxImportLevels = 256;
inline constexpr int32 kMaxImportChunksPerLayer = 65536;
inline constexpr int32 kMaxImportAnimationFrames = 1024;
inline constexpr int32 kMaxImportPolygonPoints = 256;
// Whole-import ceilings, shared across levels, layers, tilesets and external files.
inline constexpr uint64 kMaxImportSourceBytes = uint64(64) << 20;
inline constexpr uint64 kMaxImportItems = uint64(1) << 20;
inline constexpr uint64 kMaxImportCells = uint64(16) << 20;
inline constexpr uint64 kMaxImportOutputChunks = 16384;

} // namespace GameEngine::TileMap::Importers
