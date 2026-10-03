#include "app_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace map_editor::app_internal {
namespace {

constexpr Vector3 kWorldUp{0.0F, 1.0F, 0.0F};
constexpr float kMinFlySpeed = 2.0F;
constexpr float kMaxFlySpeed = 512.0F;
constexpr float kFlySpeedWheelStep = 1.1F;
constexpr float kMouseLookRadiansPerPixel = 0.0035F;
constexpr float kMaxPitchRadians = 1.55334306F; // 89 degrees.

Vector3 add(Vector3 a, Vector3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector3 subtract(Vector3 a, Vector3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3 scale(Vector3 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float dot(Vector3 a, Vector3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 cross(Vector3 a, Vector3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float length(Vector3 value) {
    return std::sqrt(dot(value, value));
}

Vector3 normalize_or(Vector3 value, Vector3 fallback) {
    const auto len = length(value);
    if (len <= std::numeric_limits<float>::epsilon()) {
        return fallback;
    }
    return scale(value, 1.0F / len);
}

Vector3 rotate_around_axis(Vector3 value, Vector3 axis, float radians) {
    axis = normalize_or(axis, kWorldUp);
    const auto c = std::cos(radians);
    const auto s = std::sin(radians);
    return add(
        add(scale(value, c), scale(cross(axis, value), s)),
        scale(axis, dot(axis, value) * (1.0F - c)));
}

} // namespace

Camera3D default_camera() {
    return {{96.0F, 80.0F, 96.0F}, {0.0F, 16.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 60.0F, CAMERA_PERSPECTIVE};
}

float adjust_camera_fly_speed(float current_speed, float wheel_delta) {
    auto speed = std::clamp(current_speed, kMinFlySpeed, kMaxFlySpeed);
    if (std::fabs(wheel_delta) <= std::numeric_limits<float>::epsilon()) {
        return speed;
    }
    speed *= std::pow(kFlySpeedWheelStep, wheel_delta);
    return std::clamp(speed, kMinFlySpeed, kMaxFlySpeed);
}

FlyInput read_fly_input() {
    // WASD follows the view (W climbs when looking up), Space/Ctrl lift and
    // sink, Shift slows, Z speeds up (Shift+Z slows further).
    return {
        IsKeyDown(KEY_W),
        IsKeyDown(KEY_S),
        IsKeyDown(KEY_A),
        IsKeyDown(KEY_D),
        IsKeyDown(KEY_SPACE),
        IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL),
        IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT),
        IsKeyDown(KEY_Z),
    };
}

void update_free_fly_camera(Camera3D& camera, float fly_speed, const FlyInput& input) {
    auto forward = normalize_or(subtract(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
    const auto mouse_delta = GetMouseDelta();
    if (mouse_delta.x != 0.0F || mouse_delta.y != 0.0F) {
        forward = normalize_or(
            rotate_around_axis(forward, kWorldUp, -mouse_delta.x * kMouseLookRadiansPerPixel),
            forward);

        const auto current_pitch = std::asin(std::clamp(forward.y, -1.0F, 1.0F));
        const auto requested_pitch = current_pitch - mouse_delta.y * kMouseLookRadiansPerPixel;
        const auto target_pitch = std::clamp(requested_pitch, -kMaxPitchRadians, kMaxPitchRadians);
        const auto right = normalize_or(cross(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
        forward = normalize_or(rotate_around_axis(forward, right, target_pitch - current_pitch), forward);
    }

    const auto delta = fly_camera_step({forward.x, forward.y, forward.z}, input,
        adjust_camera_fly_speed(fly_speed, 0.0F), GetFrameTime());
    camera.position = add(camera.position, Vector3{delta.x, delta.y, delta.z});

    camera.up = kWorldUp;
    camera.target = add(camera.position, forward);
}

Vector2 get_viewport_mouse_position(Vector2 viewport_pos, Vector2 viewport_size) {
    const auto mouse_pos = GetMousePosition();
    return Vector2{
        std::clamp(mouse_pos.x - viewport_pos.x, 0.0F, viewport_size.x),
        std::clamp(mouse_pos.y - viewport_pos.y, 0.0F, viewport_size.y),
    };
}

Ray get_viewport_mouse_ray(const Camera3D& camera, Vector2 viewport_mouse_pos, Vector2 viewport_size) {
    return GetScreenToWorldRayEx(
        viewport_mouse_pos,
        camera,
        static_cast<int>(std::max(1.0F, viewport_size.x)),
        static_cast<int>(std::max(1.0F, viewport_size.y)));
}

void update_orbit_camera(Camera3D& camera, EditorUi& ui, Vector2 viewport_mouse_pos, Vector2 viewport_size,
                         bool viewport_hovered, const MapScene* scene) {
    constexpr float kFallbackPivotDistance = 50.0F;
    const auto ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, viewport_size);
    // Terrain (or the y=0 plane) under the cursor, as the orbit/pan/zoom pivot.
    const auto ground_distance = [&]() -> std::optional<float> {
        if (scene != nullptr && scene->terrain.has_value()) {
            const auto hit = ray_terrain_hit(*scene->terrain,
                {ray.position.x, ray.position.y, ray.position.z}, {ray.direction.x, ray.direction.y, ray.direction.z});
            if (hit.has_value()) {
                return length(subtract(Vector3{hit->x, hit->y, hit->z}, ray.position));
            }
        } else if (std::fabs(ray.direction.y) > 0.0001F) {
            const auto t = -ray.position.y / ray.direction.y;
            if (t > 0.0F) return t;
        }
        return std::nullopt;
    };

    const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    if (viewport_hovered && ui.camera_drag == CameraDrag::None) {
        const bool orbit = alt && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        const bool pan = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
        if (orbit || pan) {
            ui.camera_drag = orbit ? CameraDrag::Orbit : CameraDrag::Pan;
            ui.camera_pivot_distance = ground_distance().value_or(kFallbackPivotDistance);
            ui.camera_pivot = add(ray.position, scale(ray.direction, ui.camera_pivot_distance));
            ui.focus_animating = false;
        }
    }

    const auto delta = GetMouseDelta();
    auto forward = normalize_or(subtract(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
    if (ui.camera_drag == CameraDrag::Orbit) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            ui.camera_drag = CameraDrag::None;
        } else {
            // Same rotation as free-fly mouse look, applied around the pivot.
            const auto yaw = -delta.x * kMouseLookRadiansPerPixel;
            auto offset = rotate_around_axis(subtract(camera.position, ui.camera_pivot), kWorldUp, yaw);
            forward = normalize_or(rotate_around_axis(forward, kWorldUp, yaw), forward);
            const auto current_pitch = std::asin(std::clamp(forward.y, -1.0F, 1.0F));
            const auto target_pitch = std::clamp(current_pitch - delta.y * kMouseLookRadiansPerPixel, -kMaxPitchRadians, kMaxPitchRadians);
            const auto right = normalize_or(cross(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
            offset = rotate_around_axis(offset, right, target_pitch - current_pitch);
            forward = normalize_or(rotate_around_axis(forward, right, target_pitch - current_pitch), forward);
            camera.position = add(ui.camera_pivot, offset);
            camera.target = add(camera.position, forward);
            camera.up = kWorldUp;
        }
    } else if (ui.camera_drag == CameraDrag::Pan) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            ui.camera_drag = CameraDrag::None;
        } else {
            // World units per pixel at the pivot depth, so the grabbed point
            // stays under the cursor.
            const auto units_per_pixel = 2.0F * ui.camera_pivot_distance *
                std::tan(camera.fovy * 0.5F * DEG2RAD) / std::max(1.0F, viewport_size.y);
            const auto right = normalize_or(cross(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
            const auto up = cross(right, forward);
            const auto move = add(scale(right, -delta.x * units_per_pixel), scale(up, delta.y * units_per_pixel));
            camera.position = add(camera.position, move);
            camera.target = add(camera.target, move);
        }
    }

    // Browsers and trackpads can report several notches in one event; cap a
    // frame at one notch so a single flick cannot dive through the scene.
    const auto wheel = std::clamp(GetMouseWheelMove(), -1.0F, 1.0F);
    if (viewport_hovered && ui.camera_drag == CameraDrag::None && std::fabs(wheel) > 0.0F) {
        // Each notch covers a fixed share of the distance to the point under the
        // cursor, so zoom never passes through it.
        const auto distance = ground_distance().value_or(kFallbackPivotDistance);
        const auto step = distance * (1.0F - std::pow(0.8F, wheel));
        const auto move = scale(ray.direction, step);
        camera.position = add(camera.position, move);
        camera.target = add(camera.target, move);
        ui.focus_animating = false;
    }
}

void focus_camera_on_city(Camera3D& camera, const EditorProject& project) {
    if (!project.city) {
        camera = default_camera();
        return;
    }
    constexpr float unit_size = 4.0F;
    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    float height_sum = 0.0F;
    std::size_t point_count = 0;
    const auto include = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        const float world_x = static_cast<float>(x) * unit_size;
        const float world_z = static_cast<float>(z) * unit_size;
        min_x = std::min(min_x, world_x);
        max_x = std::max(max_x, world_x);
        min_z = std::min(min_z, world_z);
        max_z = std::max(max_z, world_z);
        height_sum += static_cast<float>(y) * unit_size;
        ++point_count;
    };
    for (const auto& intersection : project.city->data.intersections) {
        include(intersection.x, intersection.y, intersection.z);
    }
    for (const auto& building : project.city->data.buildings) {
        include(building.x, building.y, building.z);
    }
    if (point_count == 0U) {
        camera = default_camera();
        return;
    }
    const float center_x = (min_x + max_x) * 0.5F;
    const float center_z = (min_z + max_z) * 0.5F;
    const float span_x = std::max(max_x - min_x, 1.0F);
    const float span_z = std::max(max_z - min_z, 1.0F);
    const float radius = 0.5F * std::sqrt(span_x * span_x + span_z * span_z);
    const float center_y = height_sum / static_cast<float>(point_count);
    // Fit the complete landscape instead of using the old fixed 96-unit offset,
    // which placed the camera inside large maps. The diagonal view keeps both
    // horizontal extents visible and leaves headroom for buildings. The
    // bounding circle overstates a flat city seen from this angle; 1.65 radii
    // fills most of the viewport (the old 2.7 left it at about half the width).
    const float distance = std::max(160.0F, radius * 1.65F);
    const Vector3 view_direction = normalize_or(Vector3{1.0F, 0.8F, 1.0F}, Vector3{1.0F, 0.0F, 1.0F});

    camera.target = Vector3{center_x, center_y, center_z};
    camera.position = add(camera.target, scale(view_direction, distance));
    camera.up = Vector3{0.0F, 1.0F, 0.0F};
    camera.fovy = 60.0F;
    camera.projection = CAMERA_PERSPECTIVE;
}

std::optional<CityGridPoint> city_grid_point_under_mouse(const Camera3D& camera, Vector2 viewport_mouse_pos, Vector2 viewport_size) {
    const auto ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, viewport_size);
    if (std::fabs(ray.direction.y) < 0.0001F) {
        return std::nullopt;
    }

    const auto distance = -ray.position.y / ray.direction.y;
    if (distance < 0.0F) {
        return std::nullopt;
    }

    constexpr auto unit_size = 4.0F;
    const auto hit = Vector3{
        ray.position.x + ray.direction.x * distance,
        0.0F,
        ray.position.z + ray.direction.z * distance,
    };
    return CityGridPoint{
        static_cast<std::int32_t>(std::lround(hit.x / unit_size)),
        0,
        static_cast<std::int32_t>(std::lround(hit.z / unit_size)),
    };
}

// Like city_grid_point_under_mouse but raycasts the actual terrain surface
// instead of the y=0 plane, so the returned grid cell follows the land up and
// down. X/Z snap to the grid; Y is the grid level of the terrain height at that
// cell. Falls back to the flat-plane picker when the scene has no terrain.
std::optional<CityGridPoint> city_grid_point_under_mouse_terrain(
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
    const MapScene& scene) {
    const auto unit = scene.unit_size > 0.0F ? scene.unit_size : 4.0F;
    if (!scene.terrain.has_value()) {
        return city_grid_point_under_mouse(camera, viewport_mouse_pos, viewport_size);
    }

    const auto ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, viewport_size);
    const Vec3 ro{ray.position.x, ray.position.y, ray.position.z};
    const Vec3 rd{ray.direction.x, ray.direction.y, ray.direction.z};
    const auto hit = ray_terrain_hit(*scene.terrain, ro, rd);
    if (!hit.has_value()) {
        return std::nullopt;
    }

    const auto gx = static_cast<std::int32_t>(std::lround(hit->x / unit));
    const auto gz = static_cast<std::int32_t>(std::lround(hit->z / unit));
    const auto ground = terrain_height_at(*scene.terrain, static_cast<float>(gx) * unit, static_cast<float>(gz) * unit);
    return CityGridPoint{
        gx,
        static_cast<std::int32_t>(std::lround(ground / unit)),
        gz,
    };
}

std::optional<std::pair<std::size_t, std::size_t>> city_block_slot_under_mouse(
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size) {
    const auto ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, viewport_size);
    if (std::fabs(ray.direction.y) < 0.0001F) {
        return std::nullopt;
    }

    const auto distance = -ray.position.y / ray.direction.y;
    if (distance < 0.0F) {
        return std::nullopt;
    }

    const auto hit_x = ray.position.x + ray.direction.x * distance;
    const auto hit_z = ray.position.z + ray.direction.z * distance;
    const auto column = static_cast<int>(std::lround(hit_x / 8.0F));
    const auto row = static_cast<int>(std::lround(hit_z / 8.0F));
    if (column < 0 || column > 3 || row < 0 || row > 1) {
        return std::nullopt;
    }
    return std::pair<std::size_t, std::size_t>{0U, static_cast<std::size_t>(row * 4 + column)};
}

bool select_building_cell_by_index(const SbbBuilding& building, EditorUi& ui, std::size_t cell_index) {
    std::size_t occupied_position = 0;
    for (std::size_t i = 0; i < building.cells.size(); ++i) {
        const auto& cell = building.cells[i];
        if (!cell.occupied) {
            continue;
        }
        if (i == cell_index || cell.index == cell_index) {
            ui.selected_building_cell = occupied_position;
            return true;
        }
        ++occupied_position;
    }
    return false;
}

bool select_building_cell_from_viewport(
    EditorProject& project,
    EditorUi& ui,
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
    std::optional<std::uint32_t> floor,
    std::string& status) {
    if (project.buildings.empty() || viewport_size.x <= 0.0F || viewport_size.y <= 0.0F) {
        return false;
    }

    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    const auto ray = GetScreenToWorldRayEx(
        viewport_mouse_pos,
        camera,
        static_cast<int>(std::max(1.0F, viewport_size.x)),
        static_cast<int>(std::max(1.0F, viewport_size.y)));
    const auto ray_origin = Vec3{ray.position.x, ray.position.y, ray.position.z};
    const auto ray_dir = Vec3{ray.direction.x, ray.direction.y, ray.direction.z};
    constexpr auto unit_size = 4.0F;

    // The block whose rendered face is under the cursor, not the cell the ray
    // starts in: the edit view's mesh carries each face's cell.
    (void)unit_size;
    (void)floor;
    const auto picked = pick_building_face(project.building_floor_preview_render_buffer, ray_origin, ray_dir);
    if (!picked.has_value()) {
        status = "nothing under the cursor";
        return false;
    }

    if (!select_building_cell_by_index(building, ui, picked->cell_index)) {
        status = "picker hit cell " + std::to_string(picked->cell_index) + " but it is not selectable";
        return false;
    }
    const auto occupied = occupied_cell_indices(building);
    if (ui.selected_building_cell < occupied.size() && occupied[ui.selected_building_cell] < building.cells.size()) {
        const auto& cell = building.cells[occupied[ui.selected_building_cell]];
        status = "selected cell " + std::to_string(cell.index) +
            " [" + std::to_string(cell.x) + "," + std::to_string(cell.y) + "," + std::to_string(cell.z) + "]";
    }
    return true;
}


} // namespace map_editor::app_internal
