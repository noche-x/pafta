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
constexpr float kFlySpeedWheelStep = 1.2F;
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

Vector3 horizontal_forward(Vector3 forward) {
    return normalize_or({forward.x, 0.0F, forward.z}, {0.0F, 0.0F, -1.0F});
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

void update_free_fly_camera(Camera3D& camera, float fly_speed) {
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

    const auto forward_xz = horizontal_forward(forward);
    const auto right_xz = normalize_or(cross(forward_xz, kWorldUp), {1.0F, 0.0F, 0.0F});
    Vector3 movement{0.0F, 0.0F, 0.0F};
    if (IsKeyDown(KEY_W)) {
        movement = add(movement, forward_xz);
    }
    if (IsKeyDown(KEY_S)) {
        movement = subtract(movement, forward_xz);
    }
    if (IsKeyDown(KEY_D)) {
        movement = add(movement, right_xz);
    }
    if (IsKeyDown(KEY_A)) {
        movement = subtract(movement, right_xz);
    }
    if (IsKeyDown(KEY_SPACE)) {
        movement = add(movement, kWorldUp);
    }
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
        movement = subtract(movement, kWorldUp);
    }

    const auto movement_len = length(movement);
    if (movement_len > std::numeric_limits<float>::epsilon()) {
        const auto ctrl_multiplier = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) ? 2.0F : 1.0F;
        const auto step = adjust_camera_fly_speed(fly_speed, 0.0F) * ctrl_multiplier * GetFrameTime();
        const auto delta = scale(movement, step / movement_len);
        camera.position = add(camera.position, delta);
    }

    camera.up = kWorldUp;
    camera.target = add(camera.position, forward);
}

Vector2 get_virtual_mouse_position(Vector2 viewport_pos, Vector2 viewport_size) {
    Vector2 mouse_pos = GetMousePosition();
    float norm_x = (mouse_pos.x - viewport_pos.x) / viewport_size.x;
    float norm_y = (mouse_pos.y - viewport_pos.y) / viewport_size.y;
    return Vector2{ norm_x * GetScreenWidth(), norm_y * GetScreenHeight() };
}

Vector2 get_viewport_mouse_position(Vector2 viewport_pos, Vector2 viewport_size) {
    const auto mouse_pos = GetMousePosition();
    return Vector2{
        std::clamp(mouse_pos.x - viewport_pos.x, 0.0F, viewport_size.x),
        std::clamp(mouse_pos.y - viewport_pos.y, 0.0F, viewport_size.y),
    };
}

void focus_camera_on_city(Camera3D& camera, const EditorProject& project) {
    if (!project.city) {
        camera = default_camera();
        return;
    }
    const auto scene = build_map_scene(project.city->data);
    const float center_x = (scene.bounds.min_x + scene.bounds.max_x) * 0.5F;
    const float center_z = (scene.bounds.min_z + scene.bounds.max_z) * 0.5F;

    camera.target = Vector3{center_x, 16.0F, center_z};
    camera.position = Vector3{center_x + 96.0F, 80.0F, center_z + 96.0F};
    camera.up = Vector3{0.0F, 1.0F, 0.0F};
    camera.fovy = 60.0F;
    camera.projection = CAMERA_PERSPECTIVE;
}

std::optional<CityGridPoint> city_grid_point_under_mouse(const Camera3D& camera, Vector2 virtual_mouse_pos) {
    const auto ray = GetMouseRay(virtual_mouse_pos, camera);
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

std::optional<std::pair<std::size_t, std::size_t>> city_block_slot_under_mouse(const Camera3D& camera, Vector2 virtual_mouse_pos) {
    const auto ray = GetMouseRay(virtual_mouse_pos, camera);
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

template <typename Building>
std::optional<std::size_t> pick_building_cell_index_if_available(
    const Building& building,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size) {
    if constexpr (requires { pick_building_cell(building, ray_origin, ray_dir, unit_size); }) {
        const auto pick = pick_building_cell(building, ray_origin, ray_dir, unit_size);
        if (pick.has_value()) {
            return pick->cell_index;
        }
    } else {
        (void)building;
        (void)ray_origin;
        (void)ray_dir;
        (void)unit_size;
        // TODO(Agent A): This compiles before scene.hpp exposes pick_building_cell.
        // Once Agent A declares it, the ADL guarded call above starts driving selection.
    }
    return std::nullopt;
}

bool select_building_cell_from_viewport(
    EditorProject& project,
    EditorUi& ui,
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
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

    const auto picked = pick_building_cell_index_if_available(building, ray_origin, ray_dir, unit_size);
    if (!picked.has_value()) {
        status = "building cell picker unavailable until scene.hpp exposes pick_building_cell";
        return false;
    }

    if (!select_building_cell_by_index(building, ui, *picked)) {
        status = "picker hit cell " + std::to_string(*picked) + " but it is not selectable";
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
