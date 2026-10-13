// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"

#include <imgui.h>
#include <raymath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace pafta::app_internal {
namespace {

constexpr Vector3 kWorldUp{0.0F, 1.0F, 0.0F};
constexpr float kMinFlySpeed = 2.0F;
constexpr float kMaxFlySpeed = 512.0F;
constexpr float kFlySpeedWheelStep = 1.1F;
constexpr float kMouseLookRadiansPerPixel = 0.0035F;
constexpr float kMaxPitchRadians = 1.55334306F; // 89 degrees.

Vector3 normalize_or(Vector3 value, Vector3 fallback) {
    const auto len = Vector3Length(value);
    if (len <= std::numeric_limits<float>::epsilon()) {
        return fallback;
    }
    return Vector3Scale(value, 1.0F / len);
}

} // namespace

Camera3D default_camera() {
    return {{96.0F, 80.0F, 96.0F}, {0.0F, 16.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 60.0F, CAMERA_PERSPECTIVE};
}

// Blender-style orthographic views. In orthographic mode fovy is the height of the
// view in world units, and the camera sits far back from its target so nothing in
// front of the target is clipped.
constexpr float kPerspectiveFovy = 60.0F;
constexpr float kOrthographicBackoff = 2000.0F;

float view_half_height(const Camera3D& camera, Vector3 point) {
    if (camera.projection == CAMERA_ORTHOGRAPHIC) return camera.fovy * 0.5F;
    return Vector3Distance(camera.position, point) * std::tan(camera.fovy * DEG2RAD * 0.5F);
}

void set_camera_orthographic(Camera3D& camera) {
    if (camera.projection == CAMERA_ORTHOGRAPHIC) return;
    const auto offset = Vector3Subtract(camera.position, camera.target);
    const auto distance = std::max(1.0F, Vector3Length(offset));
    camera.fovy = 2.0F * distance * std::tan(camera.fovy * DEG2RAD * 0.5F);
    camera.position = Vector3Add(camera.target, Vector3Scale(normalize_or(offset, {0.0F, 0.0F, 1.0F}), kOrthographicBackoff));
    camera.projection = CAMERA_ORTHOGRAPHIC;
}

void set_camera_perspective(Camera3D& camera) {
    if (camera.projection != CAMERA_ORTHOGRAPHIC) return;
    const auto direction = normalize_or(Vector3Subtract(camera.position, camera.target), {0.0F, 0.0F, 1.0F});
    // The distance at which the perspective view shows the same height.
    const float distance = std::max(1.0F, camera.fovy * 0.5F / std::tan(kPerspectiveFovy * DEG2RAD * 0.5F));
    camera.position = Vector3Add(camera.target, Vector3Scale(direction, distance));
    camera.fovy = kPerspectiveFovy;
    camera.projection = CAMERA_PERSPECTIVE;
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
    // WASD moves where you look (W climbs when you look up), Space and Ctrl go up and
    // down, Shift slows down, and Z speeds up (Shift+Z slows down even more).
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
    auto forward = normalize_or(Vector3Subtract(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
    const auto mouse_delta = GetMouseDelta();
    if (mouse_delta.x != 0.0F || mouse_delta.y != 0.0F) {
        forward = normalize_or(
            Vector3RotateByAxisAngle(forward, kWorldUp, -mouse_delta.x * kMouseLookRadiansPerPixel),
            forward);

        const auto current_pitch = std::asin(std::clamp(forward.y, -1.0F, 1.0F));
        const auto requested_pitch = current_pitch - mouse_delta.y * kMouseLookRadiansPerPixel;
        const auto target_pitch = std::clamp(requested_pitch, -kMaxPitchRadians, kMaxPitchRadians);
        const auto right = normalize_or(Vector3CrossProduct(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
        forward = normalize_or(Vector3RotateByAxisAngle(forward, right, target_pitch - current_pitch), forward);
    }

    const auto delta = fly_camera_step({forward.x, forward.y, forward.z}, input,
        adjust_camera_fly_speed(fly_speed, 0.0F), GetFrameTime());
    camera.position = Vector3Add(camera.position, Vector3{delta.x, delta.y, delta.z});

    camera.up = kWorldUp;
    camera.target = Vector3Add(camera.position, forward);
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
    // The terrain (or the y=0 plane) under the cursor, used as the pivot for orbit, pan,
    // and zoom.
    const auto ground_distance = [&]() -> std::optional<float> {
        if (scene != nullptr && scene->terrain.has_value()) {
            const auto hit = ray_terrain_hit(*scene->terrain,
                {ray.position.x, ray.position.y, ray.position.z}, {ray.direction.x, ray.direction.y, ray.direction.z});
            if (hit.has_value()) {
                return Vector3Length(Vector3Subtract(Vector3{hit->x, hit->y, hit->z}, ray.position));
            }
        } else if (std::fabs(ray.direction.y) > 0.0001F) {
            const auto t = -ray.position.y / ray.direction.y;
            if (t > 0.0F) return t;
        }
        return std::nullopt;
    };

    const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (viewport_hovered && ui.camera_drag == CameraDrag::None) {
        // Alt+left drag always orbits. The middle button pans, or with Blender-style
        // mouse it orbits, Shift pans, and Ctrl zooms.
        const bool left_orbit = alt && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        const bool middle = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
        CameraDrag drag = CameraDrag::None;
        if (left_orbit) drag = CameraDrag::Orbit;
        else if (middle && !ui.blender_mouse) drag = CameraDrag::Pan;
        else if (middle) drag = shift ? CameraDrag::Pan : ctrl ? CameraDrag::Zoom : CameraDrag::Orbit;
        // Orbiting leaves an orthographic axis view, as Blender's auto perspective does.
        if (drag == CameraDrag::Orbit) set_camera_perspective(camera);
        if (drag != CameraDrag::None) {
            ui.camera_drag = drag;
            ui.camera_drag_button = left_orbit ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_MIDDLE;
            ui.camera_pivot_distance = ground_distance().value_or(kFallbackPivotDistance);
            ui.camera_pivot = Vector3Add(ray.position, Vector3Scale(ray.direction, ui.camera_pivot_distance));
            ui.focus_animating = false;
            ui.view_snap_active = false;
        }
    }

    const auto delta = GetMouseDelta();
    auto forward = normalize_or(Vector3Subtract(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
    if (ui.camera_drag != CameraDrag::None && !IsMouseButtonDown(ui.camera_drag_button)) {
        ui.camera_drag = CameraDrag::None;
    }
    if (ui.camera_drag == CameraDrag::Zoom && camera.projection == CAMERA_ORTHOGRAPHIC) {
        camera.fovy = std::clamp(camera.fovy * std::pow(1.01F, delta.y), 0.5F, 20000.0F);
    } else if (ui.camera_drag == CameraDrag::Zoom) {
        // Dragging up moves toward the pivot, down moves away.
        const auto distance = Vector3Distance(camera.position, ui.camera_pivot);
        const auto step = std::clamp(distance * (1.0F - std::pow(0.99F, -delta.y)), -distance * 4.0F, distance * 0.9F);
        const auto move = Vector3Scale(normalize_or(Vector3Subtract(ui.camera_pivot, camera.position), forward), step);
        camera.position = Vector3Add(camera.position, move);
        camera.target = Vector3Add(camera.target, move);
    }
    if (ui.camera_drag == CameraDrag::Orbit) {
        if (!IsMouseButtonDown(ui.camera_drag_button)) {
            ui.camera_drag = CameraDrag::None;
        } else {
            // The same rotation as free-fly mouse look, applied around the pivot.
            const auto yaw = -delta.x * kMouseLookRadiansPerPixel;
            auto offset = Vector3RotateByAxisAngle(Vector3Subtract(camera.position, ui.camera_pivot), kWorldUp, yaw);
            forward = normalize_or(Vector3RotateByAxisAngle(forward, kWorldUp, yaw), forward);
            const auto current_pitch = std::asin(std::clamp(forward.y, -1.0F, 1.0F));
            const auto target_pitch = std::clamp(current_pitch - delta.y * kMouseLookRadiansPerPixel, -kMaxPitchRadians, kMaxPitchRadians);
            const auto right = normalize_or(Vector3CrossProduct(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
            offset = Vector3RotateByAxisAngle(offset, right, target_pitch - current_pitch);
            forward = normalize_or(Vector3RotateByAxisAngle(forward, right, target_pitch - current_pitch), forward);
            camera.position = Vector3Add(ui.camera_pivot, offset);
            // Keep the target at the pivot's depth, so later view changes turn around
            // what you were looking at.
            camera.target = Vector3Add(camera.position, Vector3Scale(forward, Vector3Distance(camera.position, ui.camera_pivot)));
            camera.up = kWorldUp;
        }
    } else if (ui.camera_drag == CameraDrag::Pan) {
        if (!IsMouseButtonDown(ui.camera_drag_button)) {
            ui.camera_drag = CameraDrag::None;
        } else {
            // World units per pixel at the pivot's depth, so the point you grabbed
            // stays under the cursor.
            const auto units_per_pixel = (camera.projection == CAMERA_ORTHOGRAPHIC
                ? camera.fovy
                : 2.0F * ui.camera_pivot_distance * std::tan(camera.fovy * 0.5F * DEG2RAD)) / std::max(1.0F, viewport_size.y);
            const auto right = normalize_or(Vector3CrossProduct(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
            const auto up = Vector3CrossProduct(right, forward);
            const auto move = Vector3Add(Vector3Scale(right, -delta.x * units_per_pixel), Vector3Scale(up, delta.y * units_per_pixel));
            camera.position = Vector3Add(camera.position, move);
            camera.target = Vector3Add(camera.target, move);
        }
    }

    // Browsers and trackpads can report several notches in one event. Allow at most one
    // notch per frame, so a single flick can't dive through the scene.
    const auto wheel = std::clamp(GetMouseWheelMove(), -1.0F, 1.0F);
    if (viewport_hovered && ui.camera_drag == CameraDrag::None && std::fabs(wheel) > 0.0F &&
        camera.projection == CAMERA_ORTHOGRAPHIC) {
        // Orthographic: zoom by shrinking the view, keeping the point under the cursor.
        const auto before = ray.position;
        camera.fovy = std::clamp(camera.fovy * std::pow(0.8F, wheel), 0.5F, 20000.0F);
        const auto after = get_viewport_mouse_ray(camera, viewport_mouse_pos, viewport_size).position;
        const auto shift = Vector3Subtract(before, after);
        camera.position = Vector3Add(camera.position, shift);
        camera.target = Vector3Add(camera.target, shift);
    } else if (viewport_hovered && ui.camera_drag == CameraDrag::None && std::fabs(wheel) > 0.0F) {
        // Each notch moves a fixed share of the distance to the point under the
        // cursor, so zooming never goes past it.
        const auto distance = ground_distance().value_or(kFallbackPivotDistance);
        const auto step = distance * (1.0F - std::pow(0.8F, wheel));
        const auto move = Vector3Scale(ray.direction, step);
        camera.position = Vector3Add(camera.position, move);
        camera.target = Vector3Add(camera.target, move);
        ui.focus_animating = false;
    }
}

// The point the camera turns around for view changes: its target, or for the
// free-fly camera (whose target is one unit ahead) the ground in the middle of the
// view.
Vector3 view_pivot(const Camera3D& camera) {
    const auto offset = Vector3Subtract(camera.target, camera.position);
    const auto distance = Vector3Length(offset);
    if (distance > 2.0F) return camera.target;
    const auto forward = normalize_or(offset, {0.0F, 0.0F, -1.0F});
    float reach = 60.0F;
    if (forward.y < -0.01F) reach = std::clamp(-camera.position.y / forward.y, 4.0F, 4000.0F);
    return Vector3Add(camera.position, Vector3Scale(forward, reach));
}

// Turns the camera around the view pivot until it looks from `direction` (the side
// of the pivot the camera ends on), over a short animation.
void snap_view(Camera3D& camera, EditorUi& ui, Vector3 direction) {
    set_camera_perspective(camera);
    const auto pivot = view_pivot(camera);
    const auto axis_aligned = [](Vector3 d) {
        const float largest = std::max({std::fabs(d.x), std::fabs(d.y), std::fabs(d.z)});
        return largest > 0.999F * Vector3Length(d);
    };
    ui.view_snap_ortho = axis_aligned(direction);
    // Straight up or down would leave the camera's up vector undefined.
    if (std::fabs(direction.y) > 0.999F) direction = Vector3{0.0F, direction.y > 0.0F ? 1.0F : -1.0F, 0.002F};
    ui.view_snap_from = normalize_or(Vector3Subtract(camera.position, pivot), {0.0F, 0.0F, 1.0F});
    ui.view_snap_to = normalize_or(direction, {0.0F, 0.0F, 1.0F});
    ui.view_snap_pivot = pivot;
    ui.view_snap_distance = std::max(1.0F, Vector3Distance(camera.position, pivot));
    ui.view_snap_t = 0.0F;
    ui.view_snap_active = true;
    ui.focus_animating = false;
}

void update_view_snap(Camera3D& camera, EditorUi& ui) {
    if (!ui.view_snap_active) return;
    ui.view_snap_t = std::min(1.0F, ui.view_snap_t + GetFrameTime() / 0.25F);
    const float e = ui.view_snap_t * ui.view_snap_t * (3.0F - 2.0F * ui.view_snap_t);
    // Turn along the great circle between the two directions.
    const float angle = std::acos(std::clamp(Vector3DotProduct(ui.view_snap_from, ui.view_snap_to), -1.0F, 1.0F));
    auto axis = Vector3CrossProduct(ui.view_snap_from, ui.view_snap_to);
    if (Vector3Length(axis) < 1e-4F) axis = std::fabs(ui.view_snap_from.y) < 0.9F ? kWorldUp : Vector3{1.0F, 0.0F, 0.0F};
    const auto direction = Vector3RotateByAxisAngle(ui.view_snap_from, normalize_or(axis, kWorldUp), angle * e);
    camera.position = Vector3Add(ui.view_snap_pivot, Vector3Scale(direction, ui.view_snap_distance));
    camera.target = ui.view_snap_pivot;
    camera.up = kWorldUp;
    if (ui.view_snap_t >= 1.0F) {
        ui.view_snap_active = false;
        // Like Blender, a view straight along an axis turns orthographic.
        if (ui.view_snap_ortho) set_camera_orthographic(camera);
    }
}

// Blender's navigation gizmo in the top right corner of the 3D view: the six axis
// ends drawn as the camera sees them. Click an end to look along that axis, drag
// to orbit. Numpad 1/3/7 give front/right/top (Ctrl: the opposite side), 9 flips to
// the other side, and 2/4/6/8 turn 15 degrees. Call inside the scene canvas window.
void draw_view_gizmo(Camera3D& camera, EditorUi& ui, bool keys_allowed) {
    update_view_snap(camera, ui);
    const auto pivot_turn = [&](float yaw, float pitch) {
        set_camera_perspective(camera);
        const auto pivot = view_pivot(camera);
        auto offset = Vector3Subtract(camera.position, pivot);
        offset = Vector3RotateByAxisAngle(offset, kWorldUp, yaw);
        const auto forward = normalize_or(Vector3Negate(offset), {0.0F, 0.0F, -1.0F});
        const auto current = std::asin(std::clamp(forward.y, -1.0F, 1.0F));
        const auto target = std::clamp(current + pitch, -kMaxPitchRadians, kMaxPitchRadians);
        const auto right = normalize_or(Vector3CrossProduct(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
        offset = Vector3RotateByAxisAngle(offset, right, target - current);
        camera.position = Vector3Add(pivot, offset);
        camera.target = pivot;
        camera.up = kWorldUp;
        ui.view_snap_active = false;
        ui.focus_animating = false;
    };
    if (keys_allowed && !ImGui::GetIO().WantTextInput) {
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        const float side = ctrl ? -1.0F : 1.0F;
        if (IsKeyPressed(KEY_KP_1)) snap_view(camera, ui, {0.0F, 0.0F, side});
        if (IsKeyPressed(KEY_KP_3)) snap_view(camera, ui, {side, 0.0F, 0.0F});
        if (IsKeyPressed(KEY_KP_7)) snap_view(camera, ui, {0.0F, side, 0.0F});
        if (IsKeyPressed(KEY_KP_9)) {
            const bool ortho = camera.projection == CAMERA_ORTHOGRAPHIC;
            snap_view(camera, ui, Vector3Negate(Vector3Subtract(camera.position, view_pivot(camera))));
            ui.view_snap_ortho = ortho;
        }
        if (IsKeyPressed(KEY_KP_5)) {
            ui.view_snap_active = false;
            if (camera.projection == CAMERA_ORTHOGRAPHIC) set_camera_perspective(camera);
            else set_camera_orthographic(camera);
        }
        constexpr float kStep = 15.0F * DEG2RAD;
        if (IsKeyPressed(KEY_KP_4)) pivot_turn(-kStep, 0.0F);
        if (IsKeyPressed(KEY_KP_6)) pivot_turn(kStep, 0.0F);
        if (IsKeyPressed(KEY_KP_8)) pivot_turn(0.0F, -kStep);
        if (IsKeyPressed(KEY_KP_2)) pivot_turn(0.0F, kStep);
    }

    // The gizmo is a child window, so the 3D view below it ignores its clicks.
    const float radius = 44.0F;
    const ImVec2 size{radius * 2.0F + 16.0F, radius * 2.0F + 16.0F};
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    ImGui::SetCursorScreenPos(ImVec2(window_pos.x + window_size.x - size.x - 10.0F, window_pos.y + 10.0F));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    if (ImGui::BeginChild("##view_gizmo", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##gizmo_area", size);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImVec2 center{min.x + size.x * 0.5F, min.y + size.y * 0.5F};
        auto* dl = ImGui::GetWindowDrawList();
        if (hovered || active) dl->AddCircleFilled(center, radius + 8.0F, IM_COL32(255, 255, 255, 40));
        const auto forward = normalize_or(Vector3Subtract(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
        const auto right = normalize_or(Vector3CrossProduct(forward, kWorldUp), {1.0F, 0.0F, 0.0F});
        const auto up = Vector3CrossProduct(right, forward);
        struct End { Vector3 axis; ImU32 color; const char* label; ImVec2 at; float depth; };
        std::array<End, 6> ends{{
            {{1, 0, 0}, IM_COL32(230, 70, 80, 255), "X", {}, 0.0F},
            {{0, 1, 0}, IM_COL32(120, 200, 60, 255), "Y", {}, 0.0F},
            {{0, 0, 1}, IM_COL32(70, 130, 240, 255), "Z", {}, 0.0F},
            {{-1, 0, 0}, IM_COL32(150, 60, 70, 255), nullptr, {}, 0.0F},
            {{0, -1, 0}, IM_COL32(90, 140, 50, 255), nullptr, {}, 0.0F},
            {{0, 0, -1}, IM_COL32(60, 95, 170, 255), nullptr, {}, 0.0F},
        }};
        for (auto& end : ends) {
            end.at = ImVec2(center.x + Vector3DotProduct(end.axis, right) * radius * 0.8F,
                            center.y - Vector3DotProduct(end.axis, up) * radius * 0.8F);
            end.depth = Vector3DotProduct(end.axis, forward);
        }
        // The axis end under the mouse, nearest first.
        const auto mouse = ImGui::GetIO().MousePos;
        std::optional<std::size_t> hover;
        if (hovered) {
            float best = 11.0F * 11.0F;
            for (std::size_t i = 0; i < ends.size(); ++i) {
                const float dx = mouse.x - ends[i].at.x, dy = mouse.y - ends[i].at.y;
                const float d = dx * dx + dy * dy - (ends[i].depth < 0.0F ? 1.0F : 0.0F);
                if (d < best) { best = d; hover = i; }
            }
        }
        // Back to front.
        std::array<std::size_t, 6> order{0, 1, 2, 3, 4, 5};
        std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return ends[a].depth > ends[b].depth; });
        for (const auto i : order) {
            const auto& end = ends[i];
            const bool positive = end.label != nullptr;
            if (positive) dl->AddLine(center, end.at, end.color, 2.5F);
            const float r = positive ? 9.0F : 7.0F;
            dl->AddCircleFilled(end.at, r, positive ? end.color : (end.color & 0x00FFFFFFU) | 0x90000000U);
            if (hover == i) dl->AddCircle(end.at, r + 1.5F, IM_COL32(255, 255, 255, 255), 0, 2.0F);
            if (positive) {
                const auto text = ImGui::CalcTextSize(end.label);
                dl->AddText(ImVec2(end.at.x - text.x * 0.5F, end.at.y - text.y * 0.5F), IM_COL32(20, 20, 20, 255), end.label);
            }
        }
        // Drag orbits; a click without a drag snaps to the axis under the mouse.
        static bool dragged = false;
        if (ImGui::IsItemActivated()) dragged = false;
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0F)) {
            dragged = true;
            const auto d = ImGui::GetIO().MouseDelta;
            pivot_turn(-d.x * 0.01F, -d.y * 0.01F);
        } else if (ImGui::IsItemDeactivated() && !dragged && hover) {
            snap_view(camera, ui, ends[*hover].axis);
        }
        if (hovered && !active) {
            static constexpr const char* names[] = {"Right (+X)", "Top (+Y)", "Front (+Z)", "Left (-X)", "Bottom (-Y)", "Back (-Z)"};
            ImGui::SetTooltip("%s (%s)\nClick an axis to look along it, drag to orbit.\nNumpad 1/3/7: front/right/top (Ctrl: opposite), 5: orthographic.",
                hover ? names[*hover] : "View", camera.projection == CAMERA_ORTHOGRAPHIC ? "orthographic" : "perspective");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
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
    // The diagonal view fits both horizontal sizes and leaves room above for buildings.
    // The minimum of 160 units also frames small maps from outside.
    const float distance = std::max(160.0F, radius * 1.65F);
    const Vector3 view_direction = normalize_or(Vector3{1.0F, 0.8F, 1.0F}, Vector3{1.0F, 0.0F, 1.0F});

    camera.target = Vector3{center_x, center_y, center_z};
    camera.position = Vector3Add(camera.target, Vector3Scale(view_direction, distance));
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

    // The block whose drawn face is under the cursor, not the cell the ray starts in.
    // The edit view's mesh records each face's cell.
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


} // namespace pafta::app_internal
