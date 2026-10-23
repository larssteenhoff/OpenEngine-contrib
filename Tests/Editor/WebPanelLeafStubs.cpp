// Leaf-dep stub for PanelDeferredBindLatchTests and PanelAssetLoadWithdrawalTests.
//
// WebPanel reads the editor's cross-panel asset-drag state to accept a file dropped onto
// the embedded web view. The global lives in AssetsBrowserController.cpp, which carries the
// whole Assets panel browser; neither suite dispatches a drop, so a definition is all the
// link needs.

#include "Assets/AssetGridDragState.h"

namespace GameEngine
{

AssetGridDragState g_DragState;

} // namespace GameEngine
