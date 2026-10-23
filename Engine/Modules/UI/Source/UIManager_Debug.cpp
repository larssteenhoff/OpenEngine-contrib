#include "UI/UIManager.h"
#include "UIManager_Internal.h"
#include "UI/UIElement.h"
#include "UI/ResolvedStyle.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/TreeView.h"
#include "UIAttributeAccess.h"

#include "Logger/Logger.h"

#include <iomanip>
#include <string>
#include <vector>
#include <unordered_map>

using namespace GameEngine;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA

namespace
{
constexpr size_t kGridCellSampleCount = 12;
constexpr size_t kTreeRowSampleCount = 10;

using DebugClipCache = std::unordered_map<UIElement*, UIManager::DebugClipRect>;

std::string EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out.push_back((char)c);
            }
            break;
        }
    }
    return out;
}

// JSON writes into the debug-capture stream, plus lookups into the per-element
// clip rects RebuildClipCaches recorded.
struct DebugCaptureWriter
{
    std::ostream& Out;
    const DebugClipCache& Clips;

    void WriteString(const std::string& s)
    {
        Out << "\"" << EscapeJson(s) << "\"";
    }
    void WriteBool(bool v)
    { Out << (v ? "true" : "false"); }
    bool GetClipFor(UIElement* el, float& cx, float& cy, float& cw, float& ch) const
    {
        if (!el)
            return false;
        auto it = Clips.find(el);
        if (it == Clips.end())
            return false;
        cx = it->second.X;
        cy = it->second.Y;
        cw = it->second.W;
        ch = it->second.H;
        return true;
    }
};

// First descendant of root (root itself excluded, depth-first) carrying className.
UIElement* FindDescendantWithClass(UIElement* root, const char* className)
{
    if (!root)
        return nullptr;
    static thread_local std::vector<UIElement*> stack;
    stack.clear();
    stack.reserve(16);
    stack.push_back(root);
    while (!stack.empty())
    {
        UIElement* el = stack.back();
        stack.pop_back();
        if (!el)
            continue;
        if (el != root && el->HasClass(className))
            return el;
        for (const auto& ch : el->GetChildren())
        {
            if (ch)
                stack.push_back(ch.get());
        }
    }
    return nullptr;
}

// Every element of root's subtree (root included, depth-first) carrying className.
void CollectElementsWithClass(UIElement* root, const char* className, std::vector<UIElement*>& out)
{
    static thread_local std::vector<UIElement*> stack;
    stack.clear();
    stack.reserve(128);
    stack.push_back(root);
    while (!stack.empty())
    {
        UIElement* el = stack.back();
        stack.pop_back();
        if (!el)
            continue;
        if (el->HasClass(className))
            out.push_back(el);
        for (const auto& ch : el->GetChildren())
        {
            if (ch)
                stack.push_back(ch.get());
        }
    }
}

std::string TruncateDebugText(const std::string& s)
{
    constexpr size_t kMax = 64;
    if (s.size() <= kMax)
        return s;
    return s.substr(0, kMax - 3) + "...";
}

void PushUniqueIndex(std::vector<size_t>& indices, size_t idx)
{
    for (size_t v : indices)
        if (v == idx)
            return;
    indices.push_back(idx);
}

// Emit a mix of top-visible and bottom-visible samples so captures show what
// the user sees, even when scrolled to the very bottom: up to maxSamples / 2
// from each end of the clip-intersecting samples, then a fallback fill from the
// start of all samples.
void SelectDebugSampleIndices(const std::vector<size_t>& visibleIdx, size_t sampleCount, size_t maxSamples,
                              std::vector<size_t>& sampleIndices)
{
    sampleIndices.clear();
    sampleIndices.reserve(maxSamples);
    const size_t half = maxSamples / 2;
    if (!visibleIdx.empty())
    {
        for (size_t i = 0; i < visibleIdx.size() && i < half; ++i)
            PushUniqueIndex(sampleIndices, visibleIdx[i]);
        for (size_t k = 0; k < visibleIdx.size() && k < half; ++k)
            PushUniqueIndex(sampleIndices, visibleIdx[visibleIdx.size() - 1 - k]);
    }
    for (size_t i = 0; i < sampleCount && sampleIndices.size() < maxSamples; ++i)
        PushUniqueIndex(sampleIndices, i);
}

void WriteDebugElementProbe(DebugCaptureWriter& writer, UIElement* root, const char* probeId)
{
    writer.Out << "{";
    writer.Out << "\"id\":";
    writer.WriteString(probeId ? probeId : "");

    UIElement* el = (root && probeId) ? root->FindById(HashStringId(probeId)) : nullptr;
    writer.Out << ",\"exists\":";
    writer.WriteBool(el != nullptr);
    if (el)
    {
        writer.Out << ",\"instanceId\":" << el->GetInstanceId();
        writer.Out << ",\"tag\":";
        writer.WriteString(UIAttributeAccess::GetDebugTypeName(*el));
        writer.Out << ",\"layout\":[" << el->GetLayoutX() << "," << el->GetLayoutY() << ","
                   << el->GetLayoutWidth() << "," << el->GetLayoutHeight() << "]";

        const ResolvedStyle& cs = el->GetResolvedStyle();
        const bool styleVisible = cs.Layout.DisplayMode != DisplayMode::None && cs.Visual.Visible && cs.Visual.Opacity > 0.01f;
        writer.Out << ",\"styleVisible\":";
        writer.WriteBool(styleVisible);
        writer.Out << ",\"opacity\":" << cs.Visual.Opacity;
        writer.Out << ",\"displayMode\":" << static_cast<int>(cs.Layout.DisplayMode);

        float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
        if (writer.GetClipFor(el, cx, cy, cw, ch))
        {
            writer.Out << ",\"clip\":[" << cx << "," << cy << "," << cw << "," << ch << "]";
        }
    }
    writer.Out << "}";
}

void ScanDebugCaptureTree(UIElement* root, std::vector<GridView*>& grids, std::vector<TreeView*>& trees,
                          uint32_t& sliderClassCount, uint32_t& colorPickerClassCount)
{
    grids.clear();
    trees.clear();
    if (root)
    {
        static thread_local std::vector<UIElement*> scanStack;
        scanStack.clear();
        scanStack.reserve(256);
        scanStack.push_back(root);
        while (!scanStack.empty())
        {
            UIElement* el = scanStack.back();
            scanStack.pop_back();
            if (!el)
                continue;
            if (auto* gv = dynamic_cast<GridView*>(el))
            {
                grids.push_back(gv);
            }
            if (auto* tv = dynamic_cast<TreeView*>(el))
            {
                trees.push_back(tv);
            }
            if (el->HasClass("slider"))
                ++sliderClassCount;
            if (el->HasClass("colorpicker-view") || el->HasClass("colorpicker-svbox") || el->HasClass("colorpicker-huebar"))
                ++colorPickerClassCount;
            el->ForEachLogicalChild([&](UIElement* child) { scanStack.push_back(child); });
        }
    }
}

void WriteColorPickerProbe(DebugCaptureWriter& writer, UIElement* root, uint32_t sliderClassCount,
                           uint32_t colorPickerClassCount)
{
    UIElement* colorPickerDialog = root ? root->FindById("colorpicker-dialog-picker"_sid) : nullptr;
    UIElement* colorPickerPalette = root ? root->FindById("colorpicker-palette"_sid) : nullptr;
    const size_t paletteSwatchCount = colorPickerPalette ? colorPickerPalette->GetChildren().size() : 0u;

    static constexpr const char* kColorPickerProbeIds[] =
        {
            "colorpicker-dialog-picker",
            "colorpicker-mode-toggle",
            "colorpicker-color-area",
            "colorpicker-wheel",
            "colorpicker-value-slider",
            "colorpicker-svbox",
            "colorpicker-svbox-marker",
            "colorpicker-huebar",
            "colorpicker-huebar-marker",
            "colorpicker-swatch",
            "colorpicker-alpha-value",
            "colorpicker-hdr-value",
            "colorpicker-hex",
            "colorpicker-palette-label",
            "colorpicker-palette"};
    constexpr size_t kColorPickerProbeCount = sizeof(kColorPickerProbeIds) / sizeof(kColorPickerProbeIds[0]);

    writer.Out << "\"colorPickerProbe\":{";
    writer.Out << "\"present\":";
    writer.WriteBool(colorPickerDialog != nullptr);
    writer.Out << ",";
    writer.Out << "\"sliderClassCount\":" << sliderClassCount << ",";
    writer.Out << "\"colorPickerClassCount\":" << colorPickerClassCount << ",";
    writer.Out << "\"paletteSwatchCount\":" << paletteSwatchCount << ",";
    writer.Out << "\"elements\":[";
    for (size_t i = 0; i < kColorPickerProbeCount; ++i)
    {
        if (i != 0)
            writer.Out << ",";
        WriteDebugElementProbe(writer, root, kColorPickerProbeIds[i]);
    }
    writer.Out << "]";
    writer.Out << "},";
}

struct GridCellSample
{
    UIElement* cell = nullptr;
    UIElement* label = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    // Visible intersection with the effective clip (typically the ScrollView viewport).
    float visX = 0.0f;
    float visY = 0.0f;
    float visW = 0.0f;
    float visH = 0.0f;
    bool intersectsClip = false;
};

struct GridCellCounts
{
    int Total = 0;
    int Visible = 0;
    int Invisible = 0;
    int LabelMissing = 0;
    int LabelHidden = 0;
    int LabelClipped = 0;
};

GridCellCounts CollectGridCellSamples(const DebugCaptureWriter& writer, const std::vector<UIElement*>& cells,
                                      std::vector<GridCellSample>& samples)
{
    GridCellCounts counts;
    for (UIElement* cell : cells)
    {
        if (!cell)
            continue;
        ++counts.Total;
        const ResolvedStyle& cs = cell->GetResolvedStyle();
        const bool cellVisible = cs.Layout.DisplayMode != DisplayMode::None && cs.Visual.Visible && cs.Visual.Opacity > 0.01f;
        if (cellVisible)
            ++counts.Visible;
        else
            ++counts.Invisible;

        UIElement* label = FindDescendantWithClass(cell, "grid-title");
        if (!label)
        {
            ++counts.LabelMissing;
        }
        else
        {
            const std::string text = label->GetTextContent();
            if (text.empty())
                ++counts.LabelMissing;

            const ResolvedStyle& ls = label->GetResolvedStyle();
            const bool labelVisible = ls.Layout.DisplayMode != DisplayMode::None && ls.Visual.Visible && ls.Visual.Opacity > 0.01f;
            if (!labelVisible)
                ++counts.LabelHidden;

            const float cx = cell->GetLayoutX();
            const float cy = cell->GetLayoutY();
            const float cw = cell->GetLayoutWidth();
            const float ch = cell->GetLayoutHeight();
            const float lx = label->GetLayoutX();
            const float ly = label->GetLayoutY();
            const float lw = label->GetLayoutWidth();
            const float lh = label->GetLayoutHeight();
            if (lw > 0.0f && lh > 0.0f)
            {
                const bool over =
                    (lx < cx - 0.5f) ||
                    (ly < cy - 0.5f) ||
                    (lx + lw > cx + cw + 0.5f) ||
                    (ly + lh > cy + ch + 0.5f);
                if (over)
                    ++counts.LabelClipped;
            }
            // Prefer sampling cells that actually intersect the viewport clip. This avoids
            // capturing only offscreen pool items when the grid is scrolled far down.
            float clipX = 0.0f, clipY = 0.0f, clipW = 0.0f, clipH = 0.0f;
            float visX = 0.0f, visY = 0.0f, visW = 0.0f, visH = 0.0f;
            bool intersects = false;
            if (writer.GetClipFor(cell, clipX, clipY, clipW, clipH) && clipW > 0.0f && clipH > 0.0f)
            {
                const float x0 = std::max(cx, clipX);
                const float y0 = std::max(cy, clipY);
                const float x1 = std::min(cx + cw, clipX + clipW);
                const float y1 = std::min(cy + ch, clipY + clipH);
                visX = x0;
                visY = y0;
                visW = std::max(0.0f, x1 - x0);
                visH = std::max(0.0f, y1 - y0);
                intersects = (visW > 1.0f && visH > 1.0f);
            }
            samples.push_back(GridCellSample{cell, label, cx, cy, visX, visY, visW, visH, intersects});
        }
    }

    std::sort(samples.begin(), samples.end(), [](const GridCellSample& a, const GridCellSample& b)
              {
        if (a.y == b.y)
            return a.x < b.x;
        return a.y < b.y; });
    return counts;
}

void WriteGridCellSample(DebugCaptureWriter& writer, const GridCellSample& s)
{
    UIElement* cell = s.cell;
    UIElement* label = s.label;
    const std::string cellId = cell->GetId().empty()
                                   ? (UIAttributeAccess::GetDebugTypeName(*cell) + "#" + std::to_string(cell->GetInstanceId()))
                                   : cell->GetId();
    const ResolvedStyle& cs = cell->GetResolvedStyle();
    const bool cellVisible = cs.Layout.DisplayMode != DisplayMode::None && cs.Visual.Visible && cs.Visual.Opacity > 0.01f;
    const float cellOpacity = cs.Visual.Opacity;

    const std::string labelText = label ? TruncateDebugText(label->GetTextContent()) : std::string();
    const ResolvedStyle* ls = label ? &label->GetResolvedStyle() : nullptr;
    const bool labelVisible = !ls || (ls->Layout.DisplayMode != DisplayMode::None && ls->Visual.Visible && ls->Visual.Opacity > 0.01f);
    const float labelOpacity = ls ? ls->Visual.Opacity : 1.0f;

    writer.Out << "{";
    writer.Out << "\"id\":";
    writer.WriteString(cellId);
    writer.Out << ",";
    writer.Out << "\"visible\":";
    writer.WriteBool(cellVisible);
    writer.Out << ",";
    writer.Out << "\"opacity\":" << cellOpacity << ",";
    writer.Out << "\"layout\":[" << cell->GetLayoutX() << "," << cell->GetLayoutY() << ","
               << cell->GetLayoutWidth() << "," << cell->GetLayoutHeight() << "]";
    if (s.intersectsClip)
    {
        writer.Out << ",\"visibleRect\":[" << s.visX << "," << s.visY << "," << s.visW << "," << s.visH << "]";
    }

    float cx = 0, cy = 0, cw = 0, ch = 0;
    if (writer.GetClipFor(cell, cx, cy, cw, ch))
    {
        writer.Out << ",\"clip\":[" << cx << "," << cy << "," << cw << "," << ch << "]";
    }


    if (label)
    {
        writer.Out << ",\"label\":{";
        writer.Out << "\"text\":";
        writer.WriteString(labelText);
        writer.Out << ",";
        writer.Out << "\"visible\":";
        writer.WriteBool(labelVisible);
        writer.Out << ",";
        writer.Out << "\"opacity\":" << labelOpacity << ",";
        writer.Out << "\"layout\":[" << label->GetLayoutX() << "," << label->GetLayoutY() << ","
                   << label->GetLayoutWidth() << "," << label->GetLayoutHeight() << "]";

        float lx = 0, ly = 0, lw = 0, lh = 0;
        if (writer.GetClipFor(label, lx, ly, lw, lh))
        {
            writer.Out << ",\"clip\":[" << lx << "," << ly << "," << lw << "," << lh << "]";
        }
        writer.Out << "}";
    }
    writer.Out << "}";
}

void WriteGridViewCapture(DebugCaptureWriter& writer, GridView* gv)
{
    const std::string gid = gv->GetId().empty()
                                ? (UIAttributeAccess::GetDebugTypeName(*gv) + "#" + std::to_string(gv->GetInstanceId()))
                                : gv->GetId();

    static thread_local std::vector<UIElement*> cells;
    cells.clear();
    cells.reserve(64);
    CollectElementsWithClass(gv, "grid-cell", cells);

    static thread_local std::vector<GridCellSample> samples;
    samples.clear();
    samples.reserve(cells.size());
    const GridCellCounts counts = CollectGridCellSamples(writer, cells, samples);

    writer.Out << "{";
    writer.Out << "\"id\":";
    writer.WriteString(gid);
    writer.Out << ",";
    writer.Out << "\"layout\":[" << gv->GetLayoutX() << "," << gv->GetLayoutY() << "," << gv->GetLayoutWidth()
               << "," << gv->GetLayoutHeight() << "],";
    writer.Out << "\"scrollY\":" << gv->GetScrollOffsetY() << ",";
    writer.Out << "\"viewportW\":" << gv->GetViewportWidth() << ",";
    writer.Out << "\"viewportH\":" << gv->GetViewportHeight() << ",";
    writer.Out << "\"contentW\":" << gv->GetContentWidth() << ",";
    writer.Out << "\"contentH\":" << gv->GetContentHeight() << ",";
    writer.Out << "\"columns\":" << gv->GetColumnCount() << ",";
    writer.Out << "\"firstRow\":" << gv->GetFirstVisibleRow() << ",";
    writer.Out << "\"desiredCells\":" << gv->GetDesiredCellCount() << ",";
    writer.Out << "\"lastColumns\":" << gv->GetLastKnownColumns() << ",";
    writer.Out << "\"lastContentHeightPx\":" << gv->GetLastContentHeightPx() << ",";
    writer.Out << "\"lastItemCount\":" << gv->GetLastKnownItemCount() << ",";
    writer.Out << "\"cellSummary\":{";
    writer.Out << "\"total\":" << counts.Total << ",";
    writer.Out << "\"visible\":" << counts.Visible << ",";
    writer.Out << "\"invisible\":" << counts.Invisible << ",";
    writer.Out << "\"labelMissing\":" << counts.LabelMissing << ",";
    writer.Out << "\"labelHidden\":" << counts.LabelHidden << ",";
    writer.Out << "\"labelClipped\":" << counts.LabelClipped;
    writer.Out << "},";
    writer.Out << "\"cells\":[";

    static thread_local std::vector<size_t> sampleIndices;
    static thread_local std::vector<size_t> visibleIdx;
    visibleIdx.clear();
    visibleIdx.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
    {
        if (samples[i].intersectsClip)
            visibleIdx.push_back(i);
    }
    SelectDebugSampleIndices(visibleIdx, samples.size(), kGridCellSampleCount, sampleIndices);

    for (size_t si = 0; si < sampleIndices.size(); ++si)
    {
        if (si != 0)
            writer.Out << ",";
        WriteGridCellSample(writer, samples[sampleIndices[si]]);
    }
    writer.Out << "]";
    writer.Out << "}";
}

struct TreeRowSample
{
    UIElement* row = nullptr;
    UIElement* title = nullptr;
    UIElement* foldout = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    float visX = 0.0f;
    float visY = 0.0f;
    float visW = 0.0f;
    float visH = 0.0f;
    bool intersectsClip = false;
};

void CollectTreeRowSamples(const DebugCaptureWriter& writer, const std::vector<UIElement*>& rows,
                           std::vector<TreeRowSample>& samples)
{
    for (UIElement* row : rows)
    {
        if (!row)
            continue;
        UIElement* title = FindDescendantWithClass(row, "tree-title");
        UIElement* fold = FindDescendantWithClass(row, "foldout");
        const float rx = row->GetLayoutX();
        const float ry = row->GetLayoutY();
        const float rw = row->GetLayoutWidth();
        const float rh = row->GetLayoutHeight();
        float clipX = 0.0f, clipY = 0.0f, clipW = 0.0f, clipH = 0.0f;
        float visX = 0.0f, visY = 0.0f, visW = 0.0f, visH = 0.0f;
        bool intersects = false;
        if (writer.GetClipFor(row, clipX, clipY, clipW, clipH) && clipW > 0.0f && clipH > 0.0f)
        {
            const float x0 = std::max(rx, clipX);
            const float y0 = std::max(ry, clipY);
            const float x1 = std::min(rx + rw, clipX + clipW);
            const float y1 = std::min(ry + rh, clipY + clipH);
            visX = x0;
            visY = y0;
            visW = std::max(0.0f, x1 - x0);
            visH = std::max(0.0f, y1 - y0);
            intersects = (visW > 1.0f && visH > 1.0f);
        }
        samples.push_back(TreeRowSample{row, title, fold, rx, ry, visX, visY, visW, visH, intersects});
    }
    std::sort(samples.begin(), samples.end(), [](const TreeRowSample& a, const TreeRowSample& b)
              {
        if (a.y == b.y)
            return a.x < b.x;
        return a.y < b.y; });
}

void WriteTreeRowSample(DebugCaptureWriter& writer, const TreeRowSample& s)
{
    UIElement* row = s.row;
    UIElement* title = s.title;
    const std::string rowId = row->GetId().empty()
                                  ? (UIAttributeAccess::GetDebugTypeName(*row) + "#" + std::to_string(row->GetInstanceId()))
                                  : row->GetId();
    const ResolvedStyle& rowStyle = row->GetResolvedStyle();
    const bool rowVisible = rowStyle.Layout.DisplayMode != DisplayMode::None && rowStyle.Visual.Visible && rowStyle.Visual.Opacity > 0.01f;
    const float rowOpacity = rowStyle.Visual.Opacity;
    const std::string titleText = title ? TruncateDebugText(title->GetTextContent()) : std::string();

    writer.Out << "{";
    writer.Out << "\"id\":";
    writer.WriteString(rowId);
    writer.Out << ",";
    writer.Out << "\"visible\":";
    writer.WriteBool(rowVisible);
    writer.Out << ",";
    writer.Out << "\"opacity\":" << rowOpacity << ",";
    writer.Out << "\"layout\":[" << row->GetLayoutX() << "," << row->GetLayoutY() << ","
               << row->GetLayoutWidth() << "," << row->GetLayoutHeight() << "]";
    if (s.intersectsClip)
    {
        writer.Out << ",\"visibleRect\":[" << s.visX << "," << s.visY << "," << s.visW << "," << s.visH << "]";
    }
    float cx = 0, cy = 0, cw = 0, ch = 0;
    if (writer.GetClipFor(row, cx, cy, cw, ch))
        writer.Out << ",\"clip\":[" << cx << "," << cy << "," << cw << "," << ch << "]";

    if (!titleText.empty())
    {
        writer.Out << ",\"title\":{";
        writer.Out << "\"text\":";
        writer.WriteString(titleText);
        writer.Out << ",\"layout\":[" << title->GetLayoutX() << "," << title->GetLayoutY() << ","
                   << title->GetLayoutWidth() << "," << title->GetLayoutHeight() << "]";
        float tcx = 0, tcy = 0, tcw = 0, tch = 0;
        if (writer.GetClipFor(title, tcx, tcy, tcw, tch))
            writer.Out << ",\"clip\":[" << tcx << "," << tcy << "," << tcw << "," << tch << "]";
        writer.Out << "}";
    }

    writer.Out << "}";
}

void WriteTreeViewCapture(DebugCaptureWriter& writer, TreeView* tv)
{
    const std::string tid = tv->GetId().empty()
                                ? (UIAttributeAccess::GetDebugTypeName(*tv) + "#" + std::to_string(tv->GetInstanceId()))
                                : tv->GetId();

    // Collect pooled rows under this TreeView.
    // PERF: debug capture is optional, but reuse containers when enabled.
    static thread_local std::vector<UIElement*> rows;
    rows.clear();
    rows.reserve(128);
    CollectElementsWithClass(tv, "tree-item", rows);

    writer.Out << "{";
    writer.Out << "\"id\":";
    writer.WriteString(tid);
    writer.Out << ",";
    writer.Out << "\"layout\":[" << tv->GetLayoutX() << "," << tv->GetLayoutY() << "," << tv->GetLayoutWidth()
               << "," << tv->GetLayoutHeight() << "],";
    writer.Out << "\"scrollX\":" << tv->GetScrollOffsetX() << ",";
    writer.Out << "\"scrollY\":" << tv->GetScrollOffsetY() << ",";
    writer.Out << "\"viewportW\":" << tv->GetViewportWidth() << ",";
    writer.Out << "\"viewportH\":" << tv->GetViewportHeight() << ",";
    writer.Out << "\"contentW\":" << tv->GetContentWidth() << ",";
    writer.Out << "\"contentH\":" << tv->GetContentHeight() << ",";
    writer.Out << "\"firstIndex\":" << tv->GetFirstVisibleIndex() << ",";
    writer.Out << "\"desiredRows\":" << tv->GetDesiredRowCount() << ",";
    writer.Out << "\"flatSize\":" << tv->GetLastKnownFlatSize() << ",";
    writer.Out << "\"lastViewportWPx\":" << tv->GetLastViewportWPx() << ",";

    static thread_local std::vector<TreeRowSample> samples;
    samples.clear();
    samples.reserve(rows.size());
    CollectTreeRowSamples(writer, rows, samples);

    static thread_local std::vector<size_t> sampleIndices;
    static thread_local std::vector<size_t> visibleIdx;
    visibleIdx.clear();
    visibleIdx.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        if (samples[i].intersectsClip)
            visibleIdx.push_back(i);
    SelectDebugSampleIndices(visibleIdx, samples.size(), kTreeRowSampleCount, sampleIndices);

    writer.Out << "\"rows\":[";
    for (size_t si = 0; si < sampleIndices.size(); ++si)
    {
        if (si != 0)
            writer.Out << ",";
        WriteTreeRowSample(writer, samples[sampleIndices[si]]);
    }
    writer.Out << "]";

    writer.Out << "}";
}
} // namespace


void UIManager::EmitDebugCapture()
{
    DebugCaptureWriter writer{m_DebugCaptureOut, m_DebugClipCache};

    static thread_local std::vector<GridView*> grids;
    static thread_local std::vector<TreeView*> trees;
    uint32_t sliderClassCount = 0;
    uint32_t colorPickerClassCount = 0;
    ScanDebugCaptureTree(m_Root.get(), grids, trees, sliderClassCount, colorPickerClassCount);

    m_DebugCaptureOut << std::fixed << std::setprecision(3);
    m_DebugCaptureOut << "{";
    m_DebugCaptureOut << "\"kind\":\"ui_debug_frame\",";
    m_DebugCaptureOut << "\"seq\":" << m_DebugCaptureSeq++ << ",";
    m_DebugCaptureOut << "\"time\":" << m_Time << ",";
    m_DebugCaptureOut << "\"hoverId\":";
    writer.WriteString(GetHoveredElementDebugName());
    m_DebugCaptureOut << ",";
    m_DebugCaptureOut << "\"focusId\":";
    writer.WriteString(m_FocusId);
    m_DebugCaptureOut << ",";
    m_DebugCaptureOut << "\"captureId\":";
    writer.WriteString(m_CaptureId);
    m_DebugCaptureOut << ",";

    m_DebugCaptureOut << "\"managerState\":{";
    m_DebugCaptureOut << "\"globalStylesheets\":" << m_GlobalStylesheets.size() << ",";
    m_DebugCaptureOut << "\"fileStylesheets\":" << m_FileStylesheets.size() << ",";
    m_DebugCaptureOut << "\"externalTextures\":" << m_ExternalRGRegistrations.size() << ",";
    m_DebugCaptureOut << "\"externalDeviceTextures\":" << m_ExternalDeviceTextures.size() << ",";
    m_DebugCaptureOut << "\"bgTextureCache\":" << m_BackgroundImages.EntryCount() << ",";
    m_DebugCaptureOut << "\"bgSvgMeshCache\":" << m_BackgroundSvg.CachedCount() << ",";
    m_DebugCaptureOut << "\"rootChildCount\":" << (m_Root ? m_Root->GetChildren().size() : 0u);
    m_DebugCaptureOut << "},";

    const auto renderDiag = GetRenderSlotDiagnostics();
    m_DebugCaptureOut << "\"renderSlots\":{";
    m_DebugCaptureOut << "\"activeWindowTargetId\":" << renderDiag.ActiveWindowTargetId << ",";
    m_DebugCaptureOut << "\"deviceFrameIndex\":" << renderDiag.DeviceFrameIndex << ",";
    m_DebugCaptureOut << "\"targetFrameIndex\":" << renderDiag.TargetFrameIndex << ",";
    m_DebugCaptureOut << "\"uiFrameSlot\":" << renderDiag.UiFrameSlot << ",";
    m_DebugCaptureOut << "\"rgPoolSlot\":" << renderDiag.RgPoolSlot;
    m_DebugCaptureOut << "},";

    WriteColorPickerProbe(writer, m_Root.get(), sliderClassCount, colorPickerClassCount);

    m_DebugCaptureOut << "\"gridViews\":[";
    bool firstGrid = true;
    for (auto* gv : grids)
    {
        if (!gv)
            continue;
        if (!firstGrid)
            m_DebugCaptureOut << ",";
        firstGrid = false;
        WriteGridViewCapture(writer, gv);
    }
    m_DebugCaptureOut << "],";

    m_DebugCaptureOut << "\"treeViews\":[";
    bool firstTree = true;
    for (auto* tv : trees)
    {
        if (!tv)
            continue;
        if (!firstTree)
            m_DebugCaptureOut << ",";
        firstTree = false;
        WriteTreeViewCapture(writer, tv);
    }
    m_DebugCaptureOut << "]";
    m_DebugCaptureOut << "}\n";
    m_DebugCaptureOut.flush();
}

#endif

