#include "DebugServer/SceneEntityDebugHandlers.h"

#include "DebugServer/ComponentValueReader.h"
#include "DebugServer/DebugHandlerHelpers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/ReflectedEnumValue.h"
#include "DebugServer/RenderLayerMaskValue.h"
#include "DebugServer/SceneHierarchyReport.h"
#include "DebugServer/SchemaComponentApply.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ReflectionJson.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "EditorApplication.h"
#include "EditorChangeNotifications.h"
#include "Panels/InspectorPanel.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Scene/RenderLayerMaskText.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"
#include "SceneViewController.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainEntityProvisioning.h"
#include "TerrainGrass/TerrainGrassClamps.h"
#include "Types/StringUtils.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/EntityComponentsEdit.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/SplineUndoHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

// Write one JSON value into a component's raw bytes for a single reflected field,
// dispatching on its FieldTypeId. Type-guarded so a mismatched JSON value is
// ignored rather than throwing. Float/Vec/Color accept a JSON array for the
// element-wise components (e.g. Light.Color = [r,g,b]).
// Returns false whenever the field was not written and the caller must say so: an
// AssetGuid string that did not resolve (unknown asset path), an enum value the field's
// EnumNames table does not name (unknown enumerator or undeclared integer), an entity id
// naming no live entity, or a field type this writer has no representation
// for (Mat4/Bytes/Unknown). Reporting success for a write that changed nothing is what
// this contract exists to prevent. `world` supplies entity-handle liveness.
static bool WriteFieldFromJson(ECS::World& world, std::vector<uint8_t>& bytes,
                               const ECS::FieldInfo& f, const json& v, std::string& outReason)
{
    // The integer/enum stores below memcpy a value's low bytes into the field; that assumes a
    // little-endian host (every shipping target). A big-endian port would byte-swap.
    static_assert(std::endian::native == std::endian::little, "WriteFieldFromJson assumes little-endian");
    using ECS::FieldTypeId;
    const std::size_t off = f.Offset;
    const auto fits = [&](std::size_t n)
    { return off + n <= bytes.size(); };
    const auto writeFloats = [&](std::uint32_t count)
    {
        if (!v.is_array())
            return;
        for (std::uint32_t i = 0; i < count && i < v.size(); ++i)
        {
            if (!v[i].is_number() || !fits(i * 4 + 4))
                continue;
            float x = v[i].get<float>();
            std::memcpy(bytes.data() + off + i * 4, &x, 4);
        }
    };
    const auto storeInt = [&]<class T>()
    {
        if (v.is_number() && fits(sizeof(T)))
        {
            auto x = static_cast<T>(v.get<long long>());
            std::memcpy(bytes.data() + off, &x, sizeof(T));
        }
    };
    // A reflected enum field collapses to its underlying integer Type but carries an EnumNames
    // table; ResolveEnumFieldValue turns an enumerator name or a declared integer into its value.
    if (!f.EnumNames.empty() && (v.is_string() || v.is_number()) && f.Size <= sizeof(std::int64_t) &&
        fits(f.Size))
    {
        const std::optional<std::int64_t> value = Editor::ResolveEnumFieldValue(f, v);
        if (!value)
            return false; // not an enumerator of this field → report rather than silently no-op
        std::memcpy(bytes.data() + off, &*value, f.Size); // low bytes (LE, asserted above)
        return true;
    }
    switch (f.Type)
    {
    case FieldTypeId::Bool:
        if (fits(1))
            bytes[off] = (v.is_boolean() ? v.get<bool>() : (v.is_number() && v.get<double>() != 0.0)) ? 1 : 0;
        break;
    case FieldTypeId::Int8:
        storeInt.operator()<std::int8_t>();
        break;
    case FieldTypeId::UInt8:
        storeInt.operator()<std::uint8_t>();
        break;
    case FieldTypeId::Int16:
        storeInt.operator()<std::int16_t>();
        break;
    case FieldTypeId::UInt16:
        storeInt.operator()<std::uint16_t>();
        break;
    case FieldTypeId::Int32:
        storeInt.operator()<std::int32_t>();
        break;
    case FieldTypeId::UInt32:
        storeInt.operator()<std::uint32_t>();
        break;
    case FieldTypeId::RenderLayerMask:
    {
        // A layer list ([0, 2] or the scene text "[0, 2]"), "all" or "none"; the read side reports the text.
        Rendering::RenderLayerMask layers;
        if (!fits(sizeof(layers)) || !Editor::ReadRenderLayerMaskValue(v, f.Name, layers, outReason))
            return false;
        std::memcpy(bytes.data() + off, &layers, sizeof(layers));
        return true;
    }
    case FieldTypeId::Int64:
        storeInt.operator()<std::int64_t>();
        break;
    case FieldTypeId::UInt64:
        storeInt.operator()<std::uint64_t>();
        break;
    case FieldTypeId::Double:
        if (v.is_number() && fits(8))
        {
            double x = v.get<double>();
            std::memcpy(bytes.data() + off, &x, 8);
        }
        break;
    case FieldTypeId::Float:
        if (v.is_array())
            writeFloats(f.Size / 4); // float[N] (e.g. Color[3])
        else if (v.is_number() && fits(4))
        {
            float x = v.get<float>();
            std::memcpy(bytes.data() + off, &x, 4);
        }
        break;
    case FieldTypeId::Vec2:
        writeFloats(2);
        break;
    case FieldTypeId::Vec3:
        writeFloats(3);
        break;
    case FieldTypeId::Vec4:
    case FieldTypeId::Quat:
    case FieldTypeId::Color:
        writeFloats(4);
        break;
    case FieldTypeId::String:
        if (v.is_string() && f.Size > 0 && fits(f.Size))
        {
            const std::string s = v.get<std::string>();
            const std::size_t n = std::min<std::size_t>(s.size(), f.Size - 1);
            std::memcpy(bytes.data() + off, s.data(), n);
            bytes[off + n] = 0;
        }
        break;
    case FieldTypeId::AssetGuid:
    {
        // An AssetRef<T> (MaterialRef/ModelRef/TextureRef) is a 16-byte GUID. Accept a GUID
        // string or {"guid":"..."}; the component write-back re-resolves it during extraction
        // (e.g. the renderer's MaterialRegistry::Find), so set_component can assign a material.
        const json* g = v.is_string()                           ? &v
                        : (v.is_object() && v.contains("guid"))  ? &v.at("guid")
                                                                 : nullptr;
        if (!g || !g->is_string())
            return v.is_null(); // null = leave the ref as-is (ok); any other non-string is malformed -> report
        if (!fits(GUID::kSize))
            break;
        const std::string s = g->get<std::string>();
        if (s.empty())
        {
            // "" CLEARS the reference. It is what a read emits for an unset ref, so this is the
            // half that makes get_entity_components output writable back: a payload carrying an
            // unset ref has to land as unset, not silently keep whatever the target already held.
            // (JSON null, above, means "no value supplied" and still leaves the ref alone; the
            // read shape never emits it.)
            std::memset(bytes.data() + off, 0, GUID::kSize);
            return true;
        }
        // The GUID string ctor fills zeros (not throw) on a malformed string. If it isn't a
        // raw GUID, treat the string as an asset PATH and resolve it (so MCP can assign any
        // AssetRef<> by path, e.g. "UI/Hud.uxml", matching the scene format).
        GUID guid{s};
        if (guid.IsNull())
            guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(std::filesystem::path(s));
        if (!guid.IsNull())
        {
            std::memcpy(bytes.data() + off, guid.GetData().data(), GUID::kSize);
            return true;
        }
        // Non-empty string that's neither a raw GUID nor a resolvable asset path: leave the
        // existing ref untouched and report it (restores the diagnostic the deleted UIDocument
        // special-case had) instead of a silent no-op masquerading as success.
        return false;
    }
    case FieldTypeId::EntityHandle:
    {
        if (!fits(sizeof(std::uint32_t)))
            return false;
        // JSON null clears the link. That IS a write, unlike the zero below.
        if (v.is_null())
        {
            const ECS::EntityHandle cleared{};
            std::memcpy(bytes.data() + off, &cleared.id, sizeof cleared.id);
            return true;
        }
        if (!v.is_number())
            return false;
        const auto raw = v.get<std::int64_t>();
        if (raw < 0 || raw > std::numeric_limits<std::uint32_t>::max())
            return false;
        // EntityHandle::IsValid only tests the kInvalidEntity sentinel, so a ZEROED id
        // passes it while naming no entity — versions are minted from 1, so (0,0) never
        // exists. Liveness lives in the World; ask it, or a raw byte round-trip would
        // store a handle that every reader then silently ignores.
        const ECS::EntityHandle handle{static_cast<std::uint32_t>(raw)};
        if (!world.IsValid(handle))
            return false;
        std::memcpy(bytes.data() + off, &handle.id, sizeof handle.id);
        return true;
    }
    case FieldTypeId::Mat4:
    case FieldTypeId::Bytes:
    case FieldTypeId::Unknown:
    default:
        return false; // no JSON representation here — report rather than claim a write
    }
    return true; // written, or an acceptable type-mismatch no-op
}

// Set fields on a reflected component from a JSON object, keyed by field name
// (exact or case-insensitive, so a camelCase key like "fovY" reaches the
// PascalCase field FovY). Adds the component (default-
// constructed via the factory) if missing. Returns false if the type has no
// field table.
static bool SetReflectedComponentFields(ECS::World* world, ECS::EntityHandle entity,
                                        ECS::ComponentTypeId typeId, const json& values,
                                        std::vector<std::string>* outUnapplied = nullptr)
{
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    if (fields.empty())
        return false;

    std::vector<uint8_t> bytes;
    if (!world->CaptureComponentBytes(entity, typeId, bytes))
    {
        if (!ECS::ComponentFactory::Create(*world, entity, typeId))
            return false;
        if (!world->CaptureComponentBytes(entity, typeId, bytes))
            return false;
    }

    if (values.is_object())
    {
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            bool matched = false;
            for (const ECS::FieldInfo& f : fields)
            {
                if (f.Name == it.key() || EqualsIgnoreCase(f.Name, it.key()))
                {
                    matched = true;
                    // Honor field policy: don't let set_component write fields a
                    // component marked internal (Hidden) or non-editable (ReadOnly).
                    if (ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Hidden | ECS::FieldFlags::ReadOnly))
                    {
                        if (outUnapplied)
                            outUnapplied->push_back(it.key()); // read-only/hidden: not settable here
                    }
                    else if (std::string reason; !WriteFieldFromJson(*world, bytes, f, it.value(), reason) && outUnapplied)
                    {
                        // Bad asset path, an enum value the field does not declare (listed
                        // with the values it does, so the caller can correct it), an entity
                        // id naming no live entity, a field type with no JSON form (Mat4/Bytes),
                        // or a value of the wrong form, which names the forms the field takes.
                        if (!reason.empty())
                            outUnapplied->push_back(it.key() + " (" + reason + ")");
                        else if (!f.EnumNames.empty())
                            outUnapplied->push_back(it.key() + " (valid: " + Editor::DescribeEnumFieldValues(f) + ")");
                        else
                            outUnapplied->push_back(it.key());
                    }
                    break;
                }
            }
            if (!matched && outUnapplied)
                outUnapplied->push_back(it.key()); // no field by that name on this component
        }
    }
    return world->ApplyComponentBytesImmediate(entity, typeId, bytes);
}

// Apply named field values from JSON onto a component attached to an entity.
// Adds the component if it does not already exist. Returns a RefuseRequest
// refusal on failure or a null json (is_null()) on success. Callers should check
// result.is_null() to distinguish success from error.
// Result of ApplyComponentValues. Error is null on success (the plain-json
// contract error returns already use; the implicit constructor keeps them
// terse). A branch that applies a data-only fast path sets SuppressNotify so
// set_component doesn't announce the write — the policy lives next to the
// branch that implements it. Defaulting to notify fails safe: a forgotten
// flag costs a redundant inspector rebuild, never silent UI staleness.
struct ComponentApplyResult
{
    json Error;
    bool SuppressNotify = false;

    ComponentApplyResult(json error = {}) : Error(std::move(error)) {}
};

// The one message a component write returns for keys that named no field. Shared by the
// reflected path, the hand-written branches and the create_entity terrain payload, so a
// caller cannot tell from the wording which internal route their component took.
static json UnappliedFieldsError(std::string_view operation, const std::string& componentName,
                                 const std::vector<std::string>& unapplied)
{
    std::string joined;
    for (std::size_t i = 0; i < unapplied.size(); ++i)
        joined += (i ? ", " : "") + unapplied[i];
    return Editor::RefuseRequest(std::string(operation) + " " + componentName +
                                     ": fields not applied (unknown name, read-only, "
                                     "unresolved asset/enum, dead entity id, or a field "
                                     "type this path cannot express): " +
                                     joined);
}

// The replacement for a retired pre-volume modifier name, or null when the name is
// not one. The five names are not components — only a scene FORMAT the
// migration schemas still parse — but those schemas stay registered, so a retired
// name reaching the generic schema fallback would FIND one: the lone-ApplyProperty
// path would park a half-parsed block on the entity and answer ok, and a later save
// would re-emit a legacy block that the next load turns back into a live modifier.
// Silent at the call, materialised a save later. Refused instead, naming the
// replacement (terrain-editing-ux-design 1.2).
//
// Only this surface refuses. The other schema callers — the preserved-field
// resolver and the three VCS revert sites — REPLAY already-authored data, which
// is exactly what the migration exists to consume. set_component is new authoring.
//
// Matched case-INSENSITIVELY, because every name resolver this sits in front of
// is: SceneSchemaRegistry::Find lowercases through ToKey, and
// ComponentFieldRegistry::FindByName documents case-insensitive matching for exactly this
// caller (user-typed MCP names). An exact compare here would refuse
// "TerrainFlattenModifier" and wave "terrainflattenmodifier" straight through to
// the schema it still resolves to.
static const char* RetiredModifierReplacement(const std::string& componentName)
{
    if (EqualsIgnoreCase(componentName, "TerrainFlattenModifier"))
        return "TerrainModifierVolume + TerrainFlattenEffect";
    if (EqualsIgnoreCase(componentName, "TerrainNoiseModifier"))
        return "TerrainModifierVolume + TerrainNoiseEffect";
    if (EqualsIgnoreCase(componentName, "TerrainStampModifier"))
        return "TerrainModifierVolume + TerrainStampEffect";
    if (EqualsIgnoreCase(componentName, "TerrainPaintLayerModifier"))
        return "TerrainModifierVolume + TerrainPaintLayerEffect";
    if (EqualsIgnoreCase(componentName, "TerrainSplineModifier"))
        return "TerrainModifierVolume (Shape = SplineArea) + TerrainFlattenEffect";
    return nullptr;
}

// Range invariants the reflected writer cannot know about. SetReflectedComponentFields lands raw
// bytes with no bounds checking, so a component whose fields carry ranges runs its own clamp pass
// here — the SAME pass its scene schema runs after parsing a property, so a value set over IPC and
// the same value loaded from a .scene land identically. Whole-component and idempotent, so it needs
// no knowledge of which fields the write touched.
static void ApplyReflectedComponentInvariants(ECS::World* world, ECS::EntityHandle entity,
                                              ECS::ComponentTypeId typeId)
{
    if (typeId == ECS::GetComponentTypeId<Components::TerrainGrass>())
    {
        if (auto* grass = world->GetComponentForWrite<Components::TerrainGrass>(entity))
            TerrainGrass::ClampFields(*grass);
    }
}

// PhysicsBody.motionType from its enumerator name or integer index; nullopt for anything else,
// so a typo is refused instead of landing as Dynamic.
static std::optional<Physics::MotionType> ParseMotionType(const json& value)
{
    if (value.is_string())
    {
        const std::string& name = value.get_ref<const std::string&>();
        if (name == "Static")
            return Physics::MotionType::Static;
        if (name == "Kinematic")
            return Physics::MotionType::Kinematic;
        if (name == "Dynamic")
            return Physics::MotionType::Dynamic;
        return std::nullopt;
    }
    if (value.is_number_integer())
    {
        const int64_t index = value.get<int64_t>();
        if (index >= static_cast<int64_t>(Physics::MotionType::Static) &&
            index <= static_cast<int64_t>(Physics::MotionType::Dynamic))
            return static_cast<Physics::MotionType>(index);
    }
    return std::nullopt;
}

static ComponentApplyResult ApplyComponentFields(ECS::World* world, ECS::EntityHandle entity,
                                                 const std::string& componentName, const json& values)
{
    // Every hand-written branch below understands a fixed set of field names, so it reads
    // through the tracking reader and the tail reports whatever it never looked at. Without
    // that, a misspelled or wrong-case key is indistinguishable from an absent one and
    // set_component answers `ok: true` for a write that changed nothing.
    Editor::ComponentValueReader fields(values);
    bool suppressNotify = false;

    if (componentName == "Transform")
    {
        // Merge onto the existing transform: a partial write ({position} only)
        // must not reset rotation/scale to defaults — a scale reset silently
        // collapses scale-derived volumes (reflection-probe influence boxes).
        Mathematics::Vector3 pos{};
        Mathematics::Quaternion rot{};
        Mathematics::Vector3 scl{1.0f, 1.0f, 1.0f};
        if (const auto* existing = world->GetComponent<Components::Transform>(entity))
        {
            pos = existing->GetPosition();
            rot = existing->GetRotation();
            scl = existing->GetScale();
        }
        if (fields.Has("position"))
        {
            auto& p = fields["position"];
            pos = Mathematics::Vector3(p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f));
        }
        if (fields.Has("rotation"))
        {
            auto& r = fields["rotation"];
            rot = Mathematics::Quaternion(r.value("w", 1.0f), r.value("x", 0.0f), r.value("y", 0.0f), r.value("z", 0.0f));
        }
        if (fields.Has("scale"))
        {
            auto& s = fields["scale"];
            scl = Mathematics::Vector3(s.value("x", 1.0f), s.value("y", 1.0f), s.value("z", 1.0f));
        }
        auto xf = Components::Transform::FromTRS(pos, rot, scl);
        if (!world->GetComponent<Components::Transform>(entity))
            world->AddComponentImmediate(entity, xf);
        else
            *world->GetComponentForWrite<Components::Transform>(entity) = xf;
    }
    else if (componentName == "WorldSectorCoord")
    {
        // Debug/IPC tagging surface for camera-relative rendering (Earth-scale).
        // WorldSectorCoord is also user-addable in the Inspector; this IPC path
        // is the test/automation equivalent (place a mesh far out at a precise
        // sector + small local so it renders AND picks at the same world spot).
        // Merges onto any existing sector.
        Components::WorldSectorCoord sc{};
        if (const auto* existing = world->GetComponent<Components::WorldSectorCoord>(entity))
            sc = *existing;
        sc.x = fields.Value("x", sc.x);
        sc.y = fields.Value("y", sc.y);
        sc.z = fields.Value("z", sc.z);
        if (!world->GetComponent<Components::WorldSectorCoord>(entity))
            world->AddComponentImmediate(entity, sc);
        else
            *world->GetComponentForWrite<Components::WorldSectorCoord>(entity) = sc;
    }
    else if (componentName == "Parent")
    {
        // No reflected field table (EntityHandle field, [DoNotSerialize]) — apply directly.
        // Deliberately emits no EditorChangeNotifications: this models a gameplay/script
        // reparent (the ECS data-only set fast path), which the Hierarchy panel picks up
        // via its Changed<Parent> scan, not the editor command layer. parent=0 un-parents
        // (stored as the invalid-handle sentinel, matching ReparentCommand's Parent{}).
        const uint32_t parentId = fields.Value("parent", 0u);
        const ECS::EntityHandle parentHandle =
            parentId != 0 ? ECS::EntityHandle(parentId) : ECS::EntityHandle::Invalid();
        if (parentId != 0 && !world->IsValid(parentHandle))
            return Editor::RefuseRequest("Invalid parent entity");
        if (parentHandle == entity)
            return Editor::RefuseRequest("Cannot parent an entity to itself");
        world->AddComponentImmediate(entity, Components::Parent{parentHandle});
        suppressNotify = true;
    }
    else if (componentName == "SplineComponent")
    {
        auto* splineService = SplineECS::SplineService::TryGet();
        if (!splineService)
            return Editor::RefuseRequest("SplineService not available");

        bool closed = fields.Value("closed", false);
        float defaultRadius = fields.Value("DefaultRadius", 5.0f);
        uint32_t typeVal = fields.Value("type", 0u);
        auto splineType = static_cast<Spline::SplineType>(typeVal);

        // The entity's own spline is rewritten in place, keeping its handle, so set_component's
        // spline snapshot can put the old points back on undo; a first set creates one.
        auto* existing = world->GetComponent<Components::SplineComponent>(entity);
        const bool reuse = existing && splineService->IsValid(SplineECS::SplineHandle(existing->SplineDataIndex,
                                                                                      existing->SplineDataGeneration));
        const SplineECS::SplineHandle handle =
            reuse ? SplineECS::SplineHandle(existing->SplineDataIndex, existing->SplineDataGeneration)
                  : splineService->CreateSpline(splineType, closed);
        auto* data = splineService->GetSplineData(handle);
        if (!data)
            return Editor::RefuseRequest("Failed to create spline data");
        data->Type = splineType;
        data->Closed = closed;
        data->Points.clear();

        if (fields.Has("points") && fields["points"].is_array())
        {
            for (const auto& pt : fields["points"])
            {
                float x = pt.value("x", 0.0f);
                float y = pt.value("y", 0.0f);
                float z = pt.value("z", 0.0f);
                float r = pt.value("radius", defaultRadius);
                data->AddPoint(Mathematics::Vector3(x, y, z), r);
            }
        }
        data->MarkDirty();
        Spline::RebuildSplineCache(*data);

        Components::SplineComponent sc{};
        sc.SplineDataIndex = handle.Index();
        sc.SplineDataGeneration = handle.Generation();
        sc.DefaultRadius = defaultRadius;

        if (!existing)
            world->AddComponentImmediate(entity, sc);
        else
            *world->GetComponentForWrite<Components::SplineComponent>(entity) = sc;
    }
    else if (componentName == "ParticleCollisionEventsBuffer")
    {
        if (!world->HasComponent<Components::ParticleCollisionEventsBuffer>(entity))
            world->AddComponentImmediate(entity, Components::ParticleCollisionEventsBuffer{});
        if (auto* c = world->GetComponentForWrite<Components::ParticleCollisionEventsBuffer>(entity))
            c->Clear();
    }
    else if (componentName == "PhysicsBody")
    {
        // Physics components live under PhysicsECS/Include/PhysicsECS/Components,
        // outside the component scanner's reflected glob, so they need explicit
        // handlers here (the reflected fallback below can't find them).
        // motionType is validated before anything is written, so a refused value
        // leaves the entity untouched.
        std::optional<Physics::MotionType> motionType;
        if (fields.Has("motionType"))
        {
            motionType = ParseMotionType(fields["motionType"]);
            if (!motionType)
                return Editor::RefuseRequest("PhysicsBody.motionType must be \"Static\", \"Kinematic\", "
                                             "\"Dynamic\" or 0/1/2; got " + fields["motionType"].dump());
        }
        if (!world->HasComponent<Components::PhysicsBody>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsBody{});
        auto* c = world->GetComponentForWrite<Components::PhysicsBody>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add PhysicsBody");
        if (fields.Has("mass"))
            c->mass = fields["mass"].get<float>();
        if (fields.Has("linearDamping"))
            c->linearDamping = fields["linearDamping"].get<float>();
        if (fields.Has("angularDamping"))
            c->angularDamping = fields["angularDamping"].get<float>();
        if (fields.Has("gravityScale"))
            c->gravityScale = fields["gravityScale"].get<float>();
        if (fields.Has("centerOfMassOffset") && fields["centerOfMassOffset"].is_object())
        {
            const auto& o = fields["centerOfMassOffset"];
            c->centerOfMassOffsetX = o.value("x", c->centerOfMassOffsetX);
            c->centerOfMassOffsetY = o.value("y", c->centerOfMassOffsetY);
            c->centerOfMassOffsetZ = o.value("z", c->centerOfMassOffsetZ);
        }
        if (fields.Has("centerOfMassOffsetX"))
            c->centerOfMassOffsetX = fields["centerOfMassOffsetX"].get<float>();
        if (fields.Has("centerOfMassOffsetY"))
            c->centerOfMassOffsetY = fields["centerOfMassOffsetY"].get<float>();
        if (fields.Has("centerOfMassOffsetZ"))
            c->centerOfMassOffsetZ = fields["centerOfMassOffsetZ"].get<float>();
        if (motionType)
            c->motionType = *motionType;
        c->initialized = false;
    }
    else if (componentName == "PhysicsCollider")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        auto* c = world->GetComponentForWrite<Components::PhysicsCollider>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add PhysicsCollider");
        if (fields.Has("isTrigger"))
            c->isTrigger = fields["isTrigger"].get<bool>();
    }
    else if (componentName == "BoxColliderShape")
    {
        // A collider shape implies a PhysicsCollider (required component).
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::BoxColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::BoxColliderShape{});
        auto* c = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add BoxColliderShape");
        if (fields.Has("halfExtentsX"))
            c->halfExtentsX = fields["halfExtentsX"].get<float>();
        if (fields.Has("halfExtentsY"))
            c->halfExtentsY = fields["halfExtentsY"].get<float>();
        if (fields.Has("halfExtentsZ"))
            c->halfExtentsZ = fields["halfExtentsZ"].get<float>();
    }
    else if (componentName == "SphereColliderShape")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::SphereColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::SphereColliderShape{});
        auto* c = world->GetComponentForWrite<Components::SphereColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add SphereColliderShape");
        if (fields.Has("radius"))
            c->radius = fields["radius"].get<float>();
    }
    else if (componentName == "CapsuleColliderShape")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::CapsuleColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::CapsuleColliderShape{});
        auto* c = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add CapsuleColliderShape");
        if (fields.Has("radius"))
            c->radius = fields["radius"].get<float>();
        if (fields.Has("halfHeight"))
            c->halfHeight = fields["halfHeight"].get<float>();
        if (fields.Has("axis"))
            c->axis = static_cast<uint8>(fields["axis"].get<int>());
    }
    else
    {
        // UIDocument, TerrainGrass and every other reflected component (incl. AssetRef<> fields
        // by GUID or path and enum fields by name) flow through the generic reflection path
        // below — no per-component special-case needed since WriteFieldFromJson learned
        // path-resolve + enum-by-name. A hand-written branch here would shadow the reflected
        // field table and silently cover fewer fields than the component has.
        ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName(componentName);
        std::vector<std::string> unapplied;
        if (typeId != 0 && SetReflectedComponentFields(world, entity, typeId, values, &unapplied))
        {
            // Before reporting anything: the bytes are already in the world, so a partially
            // applied write must not leave out-of-range values behind on the error path.
            ApplyReflectedComponentInvariants(world, entity, typeId);
            // The component applied, but report any keys that didn't so MCP/LLM callers
            // don't read a silent no-op as success.
            if (!unapplied.empty())
                return UnappliedFieldsError("set_component", componentName, unapplied);
            return json{};
        }
        // Schema-only components (Terrain, ...) have no reflected field table; drive them
        // through the same hand-written schema the .scene loader uses. Reflected components
        // already returned above — PostProcessVolume, SkyEnvironment and VolumetricFogEffect
        // are all reflected, so their schema entries are unreachable from here.
        if (const char* replacement = RetiredModifierReplacement(componentName))
            return Editor::RefuseRequest(componentName + " is retired; set " + replacement +
                                             " instead");
        if (const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(componentName))
        {
            const std::string refusal = Editor::ApplyComponentViaSchema(*world, entity, componentName, *schema, values);
            return refusal.empty() ? json{} : Editor::RefuseRequest(refusal);
        }
        return Editor::RefuseRequest("Unknown component: " + componentName);
    }

    // A hand-written branch ran. Anything it never asked about wrote nothing, so report it
    // in the same words the reflected path uses rather than answering `ok` for a no-op.
    if (const std::vector<std::string> unconsumed = fields.UnconsumedKeys(); !unconsumed.empty())
        return UnappliedFieldsError("set_component", componentName, unconsumed);

    ComponentApplyResult applied;
    applied.SuppressNotify = suppressNotify;
    return applied;
}

// Resolve an IPC component name to its ECS type id: reflected components via
// the field registry, schema-only ones (Terrain, PostProcessVolume, ...) via
// the ECS component-name registry (suffix match tolerates namespaced names).
static ECS::ComponentTypeId ResolveComponentTypeIdByName(const std::string& componentName)
{
    if (const ECS::ComponentTypeId reflected = ECS::ComponentFieldRegistry::FindByName(componentName);
        reflected != 0)
        return reflected;
    const std::string suffix = "::" + componentName;
    for (const auto& regName : ECS::ComponentRegistry::GetAllComponentNames())
    {
        const bool match = (regName == componentName) ||
                           (regName.size() >= suffix.size() &&
                            regName.compare(regName.size() - suffix.size(), suffix.size(), suffix) == 0);
        if (match)
            return ECS::ComponentRegistry::GetComponentTypeId(regName);
    }
    return 0;
}

// A component's on/off state is its ECS::ComponentDisabled tag, not a field, but a write
// still names it "enabled" (or "Enabled"). The key is taken out of what the field writers see
// and applied as the tag once the rest of the write landed — unless the type keeps its own
// Enabled field (ComponentRegistry::SwitchesThroughDisabledTag is false: a post-process
// effect's or a terrain effect's own value), which the field writers own.
static ComponentApplyResult ApplyComponentValues(ECS::World* world, ECS::EntityHandle entity,
                                                 const std::string& componentName, const json& values)
{
    const ECS::ComponentTypeId typeId = ResolveComponentTypeIdByName(componentName);
    if (!values.is_object() || !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return ApplyComponentFields(world, entity, componentName, values);

    json fieldValues = values;
    std::optional<bool> enabled;
    for (const char* key : {"enabled", "Enabled"})
    {
        if (!fieldValues.contains(key))
            continue;
        if (!fieldValues[key].is_boolean())
            return Editor::RefuseRequest(componentName + "." + key + " must be true or false");
        enabled = fieldValues[key].get<bool>();
        fieldValues.erase(key);
    }

    ComponentApplyResult applied = ApplyComponentFields(world, entity, componentName, fieldValues);
    if (applied.Error.is_null() && enabled)
        world->SetComponentEnabledImmediate(entity, typeId, *enabled);
    return applied;
}

static json CreateDefaultTerrainForEntity(ECS::World* world, ECS::EntityHandle entity, const json& values)
{
    if (!world || !entity.IsValid())
        return Editor::RefuseRequest("Invalid terrain entity");

    // Same key-consumption contract set_component applies: a misspelled key here would
    // silently hand back a DEFAULT terrain (a planar one for a caller who asked for a
    // planet), which is exactly the kind of no-op that reads as success.
    Editor::ComponentValueReader fields(values);

    Components::Terrain terrainComp{};
    terrainComp.SizeX = fields.Value("SizeX", 256.0f);
    terrainComp.SizeZ = fields.Value("SizeZ", 256.0f);
    terrainComp.HeightScale = fields.Value("HeightScale", 64.0f);
    terrainComp.SamplesPerMeter = fields.Value("SamplesPerMeter", terrainComp.SamplesPerMeter);
    terrainComp.StreamingRadius = fields.Value("StreamingRadius", terrainComp.StreamingRadius);
    terrainComp.Domain = fields.Value("Domain", 0) == 1
                             ? Components::TerrainDomain::Spherical
                             : Components::TerrainDomain::Planar;
    terrainComp.PlanetRadius = fields.Value("PlanetRadius", terrainComp.PlanetRadius);
    terrainComp.TargetPixelError = fields.Value("TargetPixelError", terrainComp.TargetPixelError);
    terrainComp.SeaLevel = fields.Value("SeaLevel", terrainComp.SeaLevel);
    terrainComp.CastShadows = fields.Value("CastShadows", terrainComp.CastShadows);
    terrainComp.ReceiveShadows = fields.Value("ReceiveShadows", terrainComp.ReceiveShadows);
    const bool terrainEnabled = fields.Value("Enabled", true);

    // Base relief is a companion component, attached only to a planet. The create payload
    // carries its fields as flat PlanetRelief* keys.
    Components::TerrainPlanetRelief reliefComp{};
    reliefComp.Amplitude = fields.Value("PlanetReliefAmplitude", reliefComp.Amplitude);
    reliefComp.Frequency = fields.Value("PlanetReliefFrequency", reliefComp.Frequency);
    reliefComp.Octaves = fields.Value("PlanetReliefOctaves", reliefComp.Octaves);

    if (const std::vector<std::string> unconsumed = fields.UnconsumedKeys(); !unconsumed.empty())
        return UnappliedFieldsError("create_entity", "Terrain", unconsumed);

    // Shared with the create menu (TerrainECS::ProvisionTerrainEntity says what it does: the
    // terrain's components and data, the scene's default surface-rules volume, and a refusal
    // for a size past the extent limit, which the caller gets back).
    if (std::string refusal = Editor::ProvisionTerrainEntity(*world, entity, terrainComp, reliefComp); !refusal.empty())
        return Editor::RefuseRequest(refusal);
    if (!terrainEnabled)
        ECS::Entity(world, entity).SetEnabled<Components::Terrain>(false);

    return json{};
}

// Component checker table: maps component name to a predicate.
using ComponentChecker = std::function<bool(ECS::World*, ECS::EntityHandle)>;

static const std::unordered_map<std::string, ComponentChecker>& GetComponentCheckers()
{
    static const std::unordered_map<std::string, ComponentChecker> kCheckers = {
        {"Transform", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Transform>(e); }},
        {"Name", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Name>(e); }},
        {"Parent", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Parent>(e); }},
        {"MeshRenderer", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::MeshRenderer>(e); }},
        {"Camera", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Camera>(e); }},
        {"Light", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Light>(e); }},
        {"Terrain", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Terrain>(e); }},
    };
    return kCheckers;
}

// Serialize a dynamically-discovered component to JSON via its raw bytes.
//
// A component with a reflected field table reads back BY FIELD NAME, in the canonical
// PascalCase the reflected writer accepts, so get_entity_components output feeds straight back
// into set_component (terrain-authoring-design §9.1). Enum fields carry their enumerator name
// and AssetRef<> fields their GUID as compact hex — both forms set_component parses.
// Everything else (physics, any type the scanner skipped) keeps the opaque hex blob: without a
// field table there is nothing to name it with, and guessing types would be worse than saying so.
static json SerializeDynamicComponent(ECS::World* world, ECS::EntityHandle entity,
                                      ECS::ComponentTypeId typeId)
{
    std::vector<uint8_t> bytes;
    if (!world->CaptureComponentBytes(entity, typeId, bytes))
        return json::object();
    if (bytes.empty())
        return json::object();

    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    if (!fields.empty())
    {
        std::string text;
        ECS::AppendComponentJson(text, fields, reinterpret_cast<const std::byte*>(bytes.data()));
        // The dump builds JSON text; parse once per component rather than mirror its decoding
        // ladder here. A discarded parse falls through to the hex blob below, so a decode gap
        // degrades to the opaque hex answer instead of dropping the component silently.
        if (json parsed = json::parse(text, nullptr, false); !parsed.is_discarded())
        {
            // Render layers read back as the scene text, the one form this server reports and takes.
            for (const ECS::FieldInfo& f : fields)
            {
                Rendering::RenderLayerMask layers;
                if (f.Type != ECS::FieldTypeId::RenderLayerMask || f.Size != sizeof(layers) ||
                    f.Offset + sizeof(layers) > bytes.size())
                    continue;
                std::memcpy(&layers, bytes.data() + f.Offset, sizeof(layers));
                parsed[std::string(f.Name)] = Scene::FormatRenderLayerMask(layers);
            }
            return parsed;
        }
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hexStr;
    hexStr.reserve(bytes.size() * 2);
    for (auto b : bytes)
    {
        hexStr.push_back(kHex[b >> 4]);
        hexStr.push_back(kHex[b & 0xF]);
    }
    return json{{"size", bytes.size()}, {"data", hexStr}};
}

// A component's on/off state is its ECS::ComponentDisabled tag, reported as "enabled" in its
// entry — unless the type keeps its own Enabled field (ComponentRegistry::SwitchesThroughDisabledTag
// is false), which its own dump carries.
static void AddComponentEnabledState(json& entry, ECS::World* world, ECS::EntityHandle entity,
                                     ECS::ComponentTypeId typeId)
{
    if (!entry.is_object() || !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return;
    entry["enabled"] = world->IsComponentEnabled(entity, typeId);
}

// Serialize all components of an entity to JSON.
// Hand-written serializers cover the components whose IPC shape is not their field layout
// (Transform's TRS, physics runtime handles); everything else in the entity's archetype goes
// through SerializeDynamicComponent, which names reflected fields and falls back to a hex blob
// only for types with no field table.
//
// Every hand-written block below SHADOWS the reflected shape, and each is a fixed list someone
// has to remember to extend — the same class that left TerrainGrass covering 41 of its 54 fields
// on both sides. They are not all justified by the Transform/physics reason; several are just
// older than the reflected path. Prefer deleting one on contact to adding another, and add one
// only when the component's IPC shape genuinely is not its field layout.
static json SerializeEntityComponents(ECS::World* world, ECS::EntityHandle entity)
{
    json components = json::object();

    // Track which type IDs have been serialized by the hand-written path.
    std::unordered_set<ECS::ComponentTypeId> serialized;

    if (world->HasComponent<Components::Transform>(entity))
    {
        auto* c = world->GetComponent<Components::Transform>(entity);
        auto pos = c->GetPosition();
        auto rot = c->GetRotation();
        auto scl = c->GetScale();
        const auto& rq = rot.GetGLM();
        components["Transform"] = {
            {"position", {{"x", pos.x}, {"y", pos.y}, {"z", pos.z}}},
            {"rotation", {{"x", rq.x}, {"y", rq.y}, {"z", rq.z}, {"w", rq.w}}},
            {"scale", {{"x", scl.x}, {"y", scl.y}, {"z", scl.z}}}};
        serialized.insert(ECS::GetComponentTypeId<Components::Transform>());
    }

    if (world->HasComponent<Components::Name>(entity))
    {
        auto* c = world->GetComponent<Components::Name>(entity);
        components["Name"] = {{"value", std::string(c->View())}};
        serialized.insert(ECS::GetComponentTypeId<Components::Name>());
    }

    if (world->HasComponent<Components::Parent>(entity))
    {
        auto* c = world->GetComponent<Components::Parent>(entity);
        components["Parent"] = {{"parentId", c->parent.id}};
        serialized.insert(ECS::GetComponentTypeId<Components::Parent>());
    }

    if (world->HasComponent<Components::MeshRenderer>(entity))
    {
        auto* c = world->GetComponent<Components::MeshRenderer>(entity);
        components["MeshRenderer"] = {
            {"meshId", c->meshId},
            {"renderLayers", Scene::FormatRenderLayerMask(c->RenderLayers)},
            {"castShadows", c->castShadows},
            {"receiveShadows", c->receiveShadows}};
        serialized.insert(ECS::GetComponentTypeId<Components::MeshRenderer>());
    }

    if (world->HasComponent<Components::Camera>(entity))
    {
        auto* c = world->GetComponent<Components::Camera>(entity);
        components["Camera"] = {
            {"perspective", c->Perspective},
            {"fovY", c->FovY},
            {"orthographicSize", c->OrthographicSize},
            {"nearZ", c->NearZ},
            {"farZ", c->FarZ},
            {"visibleLayers", Scene::FormatRenderLayerMask(c->VisibleLayers)},
            {"postProcessProfileId", c->PostProcessProfileId},
            {"volumeLayers", Scene::FormatRenderLayerMask(c->VolumeLayers)}};
        serialized.insert(ECS::GetComponentTypeId<Components::Camera>());
    }

    if (world->HasComponent<Components::Light>(entity))
    {
        auto* c = world->GetComponent<Components::Light>(entity);
        components["Light"] = {
            {"type", static_cast<uint32_t>(c->Type)},
            {"color", {c->Color[0], c->Color[1], c->Color[2]}},
            {"intensity", c->Intensity},
            {"range", c->Range},
            {"innerAngle", c->InnerAngle},
            {"outerAngle", c->OuterAngle},
            {"areaShape", static_cast<uint32_t>(c->AreaShape)},
            {"areaWidth", c->AreaWidth},
            {"areaHeight", c->AreaHeight},
            {"areaRadius", c->AreaRadius},
            {"falloff", static_cast<uint32_t>(c->Falloff)},
            {"decay", c->Decay},
            {"castsLight", c->CastsLight},
            {"castsShadows", c->CastsShadows},
            {"cascadeCount", c->CascadeCount},
            {"shadowAngularDiameter", c->ShadowAngularDiameter},
            {"fogContribution", c->FogContribution},
            {"fogDensityBoost", c->FogDensityBoost},
            {"fogAnisotropy", c->FogAnisotropy},
            {"fogOriginFade", c->FogOriginFade}};
        serialized.insert(ECS::GetComponentTypeId<Components::Light>());
    }

    if (world->HasComponent<Components::ParticleCollisionEventsBuffer>(entity))
    {
        auto* c = world->GetComponent<Components::ParticleCollisionEventsBuffer>(entity);
        json events = json::array();
        for (uint32_t i = 0; i < c->Count && i < Components::ParticleCollisionEventsBuffer::kMaxEvents; ++i)
        {
            const auto& e = c->Events[i];
            events.push_back({{"Emitter", e.Emitter.id},
                              {"ParticleId", e.ParticleId},
                              {"KilledParticle", e.KilledParticle},
                              {"Position", {e.Position[0], e.Position[1], e.Position[2]}},
                              {"Normal", {e.Normal[0], e.Normal[1], e.Normal[2]}},
                              {"Velocity", {e.Velocity[0], e.Velocity[1], e.Velocity[2]}},
                              {"Speed", e.Speed}});
        }
        components["ParticleCollisionEventsBuffer"] = {
            {"Count", c->Count},
            {"Events", std::move(events)}};
        serialized.insert(ECS::GetComponentTypeId<Components::ParticleCollisionEventsBuffer>());
    }

    if (world->HasComponent<Components::Terrain>(entity))
    {
        auto* c = world->GetComponent<Components::Terrain>(entity);
        components["Terrain"] = {
            {"BaseSource", static_cast<uint32>(c->BaseSource)},
            {"TerrainAsset",
             c->TerrainAssetGuid.IsNull() ? std::string{} : c->TerrainAssetGuid.ToGuid().ToString()},
            {"SizeX", c->SizeX},
            {"SizeZ", c->SizeZ},
            {"HeightScale", c->HeightScale},
            {"SamplesPerMeter", c->SamplesPerMeter},
            {"StreamingRadius", c->StreamingRadius},
            {"Domain", static_cast<uint32>(c->Domain)},
            {"PlanetRadius", c->PlanetRadius},
            {"SphereSculpt",
             c->SphereSculptGuid.IsNull() ? std::string{} : c->SphereSculptGuid.ToGuid().ToString()},
            {"TargetPixelError", c->TargetPixelError},
            {"SeaLevel", c->SeaLevel},
            {"MaterialTiling", c->MaterialTiling},
            {"MaterialLibrary", c->MaterialLibraryGuid.IsNull()
                                   ? std::string{}
                                   : c->MaterialLibraryGuid.ToGuid().ToString()},
            // The library slot each channel role binds. Per-role for the same reason the
            // per-layer keys below are per-layer: the schema takes layerRole0..3, so an
            // array key would have nothing to land on when this is fed back to set_component.
            {"LayerRole0", static_cast<uint32>(c->LayerRoleSlot[0])},
            {"LayerRole1", static_cast<uint32>(c->LayerRoleSlot[1])},
            {"LayerRole2", static_cast<uint32>(c->LayerRoleSlot[2])},
            {"LayerRole3", static_cast<uint32>(c->LayerRoleSlot[3])},
            {"LayerAlbedo0", c->LayerAlbedoTexture[0].IsNull() ? std::string{} : c->LayerAlbedoTexture[0].ToGuid().ToString()},
            {"LayerAlbedo1", c->LayerAlbedoTexture[1].IsNull() ? std::string{} : c->LayerAlbedoTexture[1].ToGuid().ToString()},
            {"LayerAlbedo2", c->LayerAlbedoTexture[2].IsNull() ? std::string{} : c->LayerAlbedoTexture[2].ToGuid().ToString()},
            {"LayerAlbedo3", c->LayerAlbedoTexture[3].IsNull() ? std::string{} : c->LayerAlbedoTexture[3].ToGuid().ToString()},
            // Per-layer, not packed: every other key here lowercases to the Terrain scene
            // schema's own property name, so get_entity_components output can be fed
            // straight back to set_component. A `LayerTiling` array and a `LayerHexTiling`
            // bitmask have no schema property to land on (it takes layerTiling0..3 and
            // layerHex0..3), which broke that round-trip for exactly these two fields.
            {"LayerTiling0", c->LayerTiling[0]},
            {"LayerTiling1", c->LayerTiling[1]},
            {"LayerTiling2", c->LayerTiling[2]},
            {"LayerTiling3", c->LayerTiling[3]},
            {"LayerHex0", (c->LayerHexTiling & 1u) != 0u},
            {"LayerHex1", (c->LayerHexTiling & 2u) != 0u},
            {"LayerHex2", (c->LayerHexTiling & 4u) != 0u},
            {"LayerHex3", (c->LayerHexTiling & 8u) != 0u},
            {"RenderLayers", Scene::FormatRenderLayerMask(c->RenderLayers)},
            {"CastShadows", c->CastShadows},
            {"ReceiveShadows", c->ReceiveShadows}};
        serialized.insert(ECS::GetComponentTypeId<Components::Terrain>());
    }

    if (world->HasComponent<Components::TerrainPlanetRelief>(entity))
    {
        auto* c = world->GetComponent<Components::TerrainPlanetRelief>(entity);
        components["TerrainPlanetRelief"] = {
            {"Amplitude", c->Amplitude},
            {"Frequency", c->Frequency},
            {"Octaves", c->Octaves}};
        serialized.insert(ECS::GetComponentTypeId<Components::TerrainPlanetRelief>());
    }

    if (world->HasComponent<Components::SplineComponent>(entity))
    {
        auto* c = world->GetComponent<Components::SplineComponent>(entity);
        components["SplineComponent"] = {
            {"SplineDataIndex", c->SplineDataIndex},
            {"SplineDataGeneration", c->SplineDataGeneration},
            {"DefaultRadius", c->DefaultRadius}};
        serialized.insert(ECS::GetComponentTypeId<Components::SplineComponent>());
    }

    // Physics components live outside the component scanner's reflected glob
    // (see the set_component handler), so the dynamic fallback below cannot name
    // their fields. The runtime handles are the interesting part here: they say
    // which physics body actually drives this entity, which is what tells a
    // duplicated or revived entity apart from one aliasing another's body.
    if (world->HasComponent<Components::PhysicsBody>(entity))
    {
        auto* c = world->GetComponent<Components::PhysicsBody>(entity);
        auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
        components["PhysicsBody"] = {
            {"motionType", static_cast<uint32_t>(c->motionType)},
            {"mass", c->mass},
            {"initialized", c->initialized},
            {"bodyHandle", c->body.Value()},
            {"shapeHandle", c->shape.Value()},
            {"childShapeCount", c->childShapeCount},
            // The physics world's own verdict on those handles: a copied handle
            // can name a live body it does not own, so "valid" is not "mine".
            {"bodyValid", pw ? pw->IsBodyValid(c->body) : false},
            {"shapeValid", pw ? pw->IsShapeValid(c->shape) : false},
            {"physicsWorldPresent", pw != nullptr}};
        serialized.insert(ECS::GetComponentTypeId<Components::PhysicsBody>());
    }

    if (world->HasComponent<Components::CharacterController>(entity))
    {
        auto* c = world->GetComponent<Components::CharacterController>(entity);
        auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
        components["CharacterController"] = {
            {"motor", static_cast<uint32_t>(c->motor)},
            {"radius", c->radius},
            {"height", c->height},
            {"initialized", c->initialized},
            {"grounded", c->grounded},
            {"characterHandle", c->character.Value()},
            {"characterValid", pw ? pw->IsCharacterValid(c->character) : false},
            {"physicsWorldPresent", pw != nullptr}};
        serialized.insert(ECS::GetComponentTypeId<Components::CharacterController>());
    }

    // Dynamic fallback: serialize remaining components via the ComponentRegistry,
    // then report every component's own on/off state beside its fields.
    auto* archetype = world->GetEntityArchetype(entity);
    if (archetype)
    {
        for (auto typeId : archetype->GetSignature().GetComponents())
        {
            std::string name;
            if (const std::string_view reflectedName = ECS::ComponentFieldRegistry::GetCanonicalName(typeId);
                !reflectedName.empty())
            {
                name.assign(reflectedName);
            }
            else if (const auto* info = ECS::ComponentRegistry::GetComponentInfo(typeId))
            {
                name = info->Name;
            }
            else
            {
                name = "Component_" + std::to_string(typeId);
            }
            // A component's switched-off tag is reported as its "enabled" below, not as an
            // entry of its own.
            if (name.starts_with(ECS::kComponentDisabledNamePrefix))
                continue;

            // Use short name (strip namespace prefix) for JSON keys.
            auto lastColon = name.rfind("::");
            if (lastColon != std::string::npos)
                name = name.substr(lastColon + 2);
            if (!serialized.count(typeId))
                components[name] = SerializeDynamicComponent(world, entity, typeId);
            AddComponentEnabledState(components[name], world, entity, typeId);
        }
    }

    return components;
}

// create_entity's write: a new entity with its name, its transform from the top-level
// position/rotation/scale, its parent and the requested components. Returns a refusal or a
// null json; `outEntity` is the new entity either way. Every component lands immediately, so
// the undo step committed after it captures them.
static json BuildRequestedEntity(ECS::World* world, const json& params, ECS::EntityHandle parentHandle,
                                 ECS::EntityHandle& outEntity)
{
    auto entity = world->Create();
    outEntity = entity.GetHandle();

    // Set name if provided.
    std::string name = params.value("name", "");
    if (!name.empty())
    {
        Components::Name nameComp{};
        std::strncpy(nameComp.value, name.c_str(), sizeof(nameComp.value) - 1);
        nameComp.value[sizeof(nameComp.value) - 1] = '\0';
        world->AddComponentImmediate(entity.GetHandle(), nameComp);
    }

    // Always add a Transform. Top-level position/rotation/scale default to identity.
    Mathematics::Vector3 pos{};
    Mathematics::Quaternion rot{};
    Mathematics::Vector3 scl{1.0f, 1.0f, 1.0f};
    if (params.contains("position") && params["position"].is_object())
    {
        auto& p = params["position"];
        pos = Mathematics::Vector3(p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f));
    }
    if (params.contains("rotation") && params["rotation"].is_object())
    {
        auto& r = params["rotation"];
        rot = Mathematics::Quaternion(r.value("w", 1.0f), r.value("x", 0.0f), r.value("y", 0.0f), r.value("z", 0.0f));
    }
    if (params.contains("scale") && params["scale"].is_object())
    {
        auto& s = params["scale"];
        scl = Mathematics::Vector3(s.value("x", 1.0f), s.value("y", 1.0f), s.value("z", 1.0f));
    }
    world->AddComponentImmediate(entity.GetHandle(), Components::Transform::FromTRS(pos, rot, scl));

    if (parentHandle.IsValid())
        world->AddComponentImmediate(entity.GetHandle(), Components::Parent{parentHandle});

    // Set components if provided.
    if (params.contains("components") && params["components"].is_object())
    {
        auto& comps = params["components"];
        if (comps.contains("Terrain"))
        {
            const auto& terrainValues = comps["Terrain"].is_object() ? comps["Terrain"] : json::object();
            auto err = CreateDefaultTerrainForEntity(world, entity.GetHandle(), terrainValues);
            if (!err.is_null())
                return err;
        }

        for (auto it = comps.begin(); it != comps.end(); ++it)
        {
            // Transform is already set from top-level position/rotation/scale.
            if (it.key() == "Transform" || it.key() == "Terrain")
                continue;
            const auto& values = it.value().is_object() ? it.value() : json::object();
            // SuppressNotify is irrelevant here: create_entity announces the
            // whole batch with one WorldStructureChanged.
            auto applied = ApplyComponentValues(world, entity.GetHandle(), it.key(), values);
            if (!applied.Error.is_null())
                return applied.Error;
        }
    }
    return json{};
}

// A snapshot edit of the spline `entity` already has, for a set_component SplineComponent that
// rewrites it in place; inactive when the entity has no live spline.
static Editor::UndoRedoService::InteractiveEdit BeginExistingSplineEdit(Editor::UndoRedoService& undo, ECS::World* world,
                                                                        ECS::EntityHandle entity,
                                                                        Editor::EditorChangeNotifications* notifications,
                                                                        const std::string& label)
{
    auto* service = SplineECS::SplineService::TryGet();
    const auto* spline = world->GetComponent<Components::SplineComponent>(entity);
    if (!service || !spline)
        return {};
    const SplineECS::SplineHandle handle(spline->SplineDataIndex, spline->SplineDataGeneration);
    if (!service->IsValid(handle))
        return {};
    return undo.BeginInteractiveEdit(
        label, Editor::SplineUndo::MakeSplineEditableSnapshotTarget(service, handle, world, entity, notifications, label));
}

// set_component's write as one undo step (with `undo`): the entity's components before and after
// (CommitEntityComponentsEdit) and, for a SplineComponent, the points of the spline it rewrites
// in place, which live in the spline service rather than in component bytes. A write refused
// part-way records the keys it applied before refusing; one refused before writing records nothing.
static ComponentApplyResult CommitComponentWrite(Editor::UndoRedoService* undo,
                                                 Editor::EditorChangeNotifications* notifications, ECS::World* world,
                                                 ECS::EntityHandle entity, const std::string& componentName,
                                                 const json& values)
{
    const std::string label = "Set " + componentName;
    ComponentApplyResult applied;
    const auto write = [&]() {
        (void)Editor::CommitEntityComponentsEdit(*world, entity, undo, notifications, label, [&]() {
            applied = ApplyComponentValues(world, entity, componentName, values);
            return applied.Error.is_null();
        });
    };
    if (!undo)
    {
        write();
        return applied;
    }
    Editor::CommitGenericEdit(*world, *undo, label, [&]() {
        Editor::UndoRedoService::InteractiveEdit splineEdit;
        if (componentName == "SplineComponent")
            splineEdit = BeginExistingSplineEdit(*undo, world, entity, notifications, label);
        write();
        splineEdit.Commit();
    });
    return applied;
}

// BuildRequestedEntity, or nothing: a refused build removes the entity it began, so the refusal
// leaves the world as it was before the request.
static json BuildRequestedEntityOrNone(ECS::World* world, const json& params, ECS::EntityHandle parentHandle,
                                       ECS::EntityHandle& outEntity)
{
    json refusal = BuildRequestedEntity(world, params, parentHandle, outEntity);
    if (!refusal.is_null() && world->IsValid(outEntity))
        world->DestroyEntityImmediate(outEntity);
    return refusal;
}

void RegisterSceneEntityDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // 3. get_scene_hierarchy
    server.RegisterHandler("get_scene_hierarchy", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return json{{"entities", json::array()}};
        return Editor::DescribeSceneHierarchy(*world); });

    // 4. get_entity_components
    server.RegisterHandler("get_entity_components", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world || !world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        json result;
        result["entityId"] = entityId;
        result["components"] = SerializeEntityComponents(world, entity);
        return result; });

    // 5. query_ecs
    server.RegisterHandler("query_ecs", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        std::string componentName = ctx.params.value("hasComponent", "");
        int limit = ctx.params.value("limit", 50);

        if (componentName.empty())
            return Editor::RefuseRequest("Missing hasComponent parameter");

        // Try the fast static checker table first, then fall back to reflected
        // field metadata and the older ComponentRegistry.
        auto& checkers = GetComponentCheckers();
        auto it = checkers.find(componentName);

        // Dynamic fallback: collect ALL type IDs whose registered name matches.
        // There can be multiple (e.g. C++ auto-registered + scripting blob component).
        std::vector<ECS::ComponentTypeId> matchingTypeIds;
        if (it == checkers.end())
        {
            if (const ECS::ComponentTypeId reflectedTypeId =
                    ECS::ComponentFieldRegistry::FindByName(componentName);
                reflectedTypeId != 0)
            {
                matchingTypeIds.push_back(reflectedTypeId);
            }

            const std::string suffix = "::" + componentName;
            for (const auto& regName : ECS::ComponentRegistry::GetAllComponentNames())
            {
                bool match = (regName == componentName) ||
                             (regName.size() >= suffix.size() &&
                              regName.compare(regName.size() - suffix.size(), suffix.size(), suffix) == 0);
                if (match)
                {
                    auto tid = ECS::ComponentRegistry::GetComponentTypeId(regName);
                    if (tid != 0)
                        matchingTypeIds.push_back(tid);
                }
            }
            if (matchingTypeIds.empty())
                return Editor::RefuseRequest("Unknown component: " + componentName);
        }

        auto entities = world->GetAliveEntitiesSnapshot();
        json results = json::array();
        int count = 0;

        for (auto entity : entities)
        {
            if (count >= limit)
                break;

            bool has = false;
            if (it != checkers.end())
            {
                has = it->second(world, entity);
            }
            else
            {
                auto* archetype = world->GetEntityArchetype(entity);
                if (archetype)
                {
                    const auto& sig = archetype->GetSignature();
                    for (auto tid : matchingTypeIds)
                    {
                        if (sig.Contains(tid))
                        {
                            has = true;
                            break;
                        }
                    }
                }
            }

            if (has)
            {
                json entry;
                entry["id"] = entity.id;
                entry["name"] = EntityNameFromWorld(world, entity);
                results.push_back(std::move(entry));
                ++count;
            }
        }

        return json{{"entities", std::move(results)}, {"total", count}}; });

    // 8. create_entity
    server.RegisterHandler("create_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        // Validate before Create so a bad request doesn't leave an orphan entity behind.
        ECS::EntityHandle parentHandle = ECS::EntityHandle::Invalid();
        if (ctx.params.contains("parentId"))
        {
            const uint32_t parentId = ctx.params.value("parentId", 0u);
            parentHandle = ECS::EntityHandle(parentId);
            if (parentId == 0 || !world->IsValid(parentHandle))
                return Editor::RefuseRequest("Invalid parentId");
        }

        // A port edit is one undo step: the entity is built and committed as already applied
        // (the markup_create precedent), so undo removes it and redo revives the same handle.
        // A refused build removes what it began and records nothing.
        constexpr const char* kCreateEntityLabel = "Create Entity";
        ECS::EntityHandle entity{};
        json refusal;
        if (app.m_UndoRedo)
        {
            Editor::CommitGenericEdit(*world, *app.m_UndoRedo, kCreateEntityLabel, [&]() {
                refusal = BuildRequestedEntityOrNone(world, ctx.params, parentHandle, entity);
                if (!refusal.is_null())
                    return;
                app.m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::DuplicateEntitiesCommand>(
                    kCreateEntityLabel, world, app.m_ChangeNotifications.get(), std::vector<ECS::EntityHandle>{entity},
                    nullptr, nullptr));
            });
        }
        else
        {
            refusal = BuildRequestedEntityOrNone(world, ctx.params, parentHandle, entity);
        }
        if (!refusal.is_null())
            return refusal;

        // Notify the editor so the hierarchy panel refreshes.
        if (app.m_ChangeNotifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = world;
            e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            app.m_ChangeNotifications->NotifyWorldStructureChanged(e);
        }

        return json{{"entityId", entity.id}}; });

    // 9. set_component
    server.RegisterHandler("set_component", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId") || !ctx.params.contains("component"))
            return Editor::RefuseRequest("Missing entityId or component parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        std::string componentName = ctx.params["component"].get<std::string>();

        // Accept values as an object or default to empty object (add component with defaults).
        json values = json::object();
        if (ctx.params.contains("values"))
        {
            auto& v = ctx.params["values"];
            if (v.is_object())
                values = v;
            else if (v.is_string())
            {
                // LLMs sometimes stringify the JSON object — parse it.
                auto parsed = json::parse(v.get<std::string>(), nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object())
                    return Editor::RefuseRequest("values must be a JSON object (or a JSON string encoding one)");
                values = std::move(parsed);
            }
        }

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        ECS::EntityHandle entity(entityId);
        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        // A port edit is one undo step (CommitComponentWrite): the components the write added
        // or changed (a collider shape brings its PhysicsCollider) and a rewritten spline's points.
        const ComponentApplyResult applied = CommitComponentWrite(app.m_UndoRedo.get(), app.m_ChangeNotifications.get(),
                                                                  world, entity, componentName, values);
        if (!applied.Error.is_null())
            return applied.Error;

        // Notify like the editor's own edit paths so event-driven UI (inspector
        // fields, tool availability gates) reacts to debug-driven writes.
        // UndoRedo kind = "values changed outside the inspector UI; rebuild"
        // (the drop-command precedent). Data-only fast paths (Parent) opt out
        // via the apply result — the policy lives in ApplyComponentValues next
        // to the branch that implements it.
        if (app.m_ChangeNotifications && !applied.SuppressNotify)
        {
            Editor::EditorChangeNotifications::ComponentChangedEvent e{};
            e.world = world;
            e.entity = entity;
            e.componentType = ResolveComponentTypeIdByName(componentName);
            e.kind = Editor::EditorChangeNotifications::ChangeKind::UndoRedo;
            app.m_ChangeNotifications->NotifyComponentChanged(e);
        }

        return json{{"ok", true}}; });

    // set_entity_enabled — toggle an entity's active state exactly like the
    // Hierarchy panel's enable/disable icon (add/remove the ECS::Disabled tag).
    // This is a structural change, announced via WorldStructureChanged — the same
    // signal path as SetHierarchyEntityEnabled — with no ComponentChanged notify,
    // so automation reproduces the real user toggle rather than the richer
    // set_component path. Cascades to descendants by default like the panel's
    // icon (pass subtree=false for the single entity); unlike the panel it is an
    // absolute setter with no enable-state memory, so re-enabling force-enables
    // descendants that were individually disabled beforehand, and it records no
    // undo entry.
    server.RegisterHandler("set_entity_enabled", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        const uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        const bool enabled = ctx.params.value("enabled", true);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const ECS::EntityHandle entity(entityId);
        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        const ECS::EntityHandle entities[] = {entity};
        Editor::CommitEntityEnabledToggle(*world, app.m_UndoRedo.get(), app.m_ChangeNotifications.get(), entities,
                                          enabled);
        return json{{"ok", true}, {"enabled", enabled}}; });

    // 10. delete_entity — routes through the editor undo system exactly like the
    // Hierarchy / Scene-View delete key (DeleteEntitiesCommand over the collected
    // subtree, preserve-handle). This lands on the undo stack so a subsequent
    // `undo` / `redo` reproduces the real user path (needed for the camera-delete
    // → undo repro; a plain DestroyEntityImmediate could not be undone).
    server.RegisterHandler("delete_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        std::vector<ECS::EntityHandle> ents =
            Editor::DeleteEntitiesCommand::CollectSubtree(*world, entity);
        if (ents.empty())
            return Editor::RefuseRequest("Entity resolved to an empty subtree");

        const uint64_t deletedCount = static_cast<uint64_t>(ents.size());
        const std::string undoName = (ents.size() > 1) ? "Delete Entities" : "Delete Entity";

        if (app.m_UndoRedo)
        {
            app.m_UndoRedo->Execute(std::make_unique<Editor::DeleteEntitiesCommand>(
                undoName, world, app.m_ChangeNotifications.get(), std::move(ents)));
        }
        else
        {
            Editor::DeleteEntitiesCommand cmd(
                undoName, world, app.m_ChangeNotifications.get(), std::move(ents));
            cmd.Redo();
        }

        return json{{"ok", true},
                    {"deletedCount", deletedCount},
                    {"undoTracked", app.m_UndoRedo != nullptr},
                    {"undoName", undoName},
                    {"canUndo", app.CanUndo()}}; });

    // 11. select_entity
    server.RegisterHandler("select_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world || !world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        // Route through SceneViewController which updates gizmos + Inspector.
        if (!app.m_Windows.empty() && app.m_Windows[0]->scene)
        {
            app.m_Windows[0]->scene->OnEntityPicked(entity);
            return json{{"ok", true}, {"entityId", entityId}};
        }

        // Fallback: update the InspectorPanel directly.
        auto* inspector = FindPanel<InspectorPanel>(app.m_PanelStorage);
        if (inspector)
        {
            inspector->ShowEntity(world, entity, /*force=*/false, /*keepMultiSelection=*/false);
            return json{{"ok", true}, {"entityId", entityId}};
        }

        return Editor::RefuseRequest("No scene view or inspector available"); });
}

} // namespace GameEngine
