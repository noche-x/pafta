#pragma once

#include "formats.hpp"
#include "scene.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace map_editor {

enum class SbbCellField {
    ExteriorBlock,
    InteriorBlock,
    CellType,
    ExteriorPortalFace,
    InteriorPortalFace,
    PlacedFlag,
    ItemSet,
};

enum class SbcBuildingField {
    X,
    Y,
    Z,
    Rotation,
};

class SbbEditorState {
public:
    explicit SbbEditorState(SbbBuilding building);

    [[nodiscard]] const SbbBuilding& building() const;
    [[nodiscard]] std::size_t selected_index() const;
    [[nodiscard]] SbbCellField active_field() const;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] const SbbCell* selected_cell() const;

    void set_active_field(SbbCellField field);
    void select_next_occupied();
    void select_previous_occupied();
    void adjust_active_field(std::int32_t delta);

private:
    SbbBuilding building_;
    std::size_t selected_index_{};
    SbbCellField active_field_{SbbCellField::ExteriorBlock};
    bool dirty_{};
};

class SbcEditorState {
public:
    explicit SbcEditorState(SbcMap map);

    [[nodiscard]] const SbcMap& map() const;
    [[nodiscard]] std::size_t selected_index() const;
    [[nodiscard]] SbcBuildingField active_field() const;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] const SbcBuilding* selected_building() const;

    void set_active_field(SbcBuildingField field);
    void select_next_building();
    void select_previous_building();
    void adjust_active_field(std::int32_t delta);

private:
    SbcMap map_;
    std::size_t selected_index_{};
    SbcBuildingField active_field_{SbcBuildingField::X};
    bool dirty_{};
};

class SblEditorState {
public:
    explicit SblEditorState(std::vector<std::uint8_t> bytes);

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const;
    [[nodiscard]] std::size_t selected_word_index() const;
    [[nodiscard]] std::uint32_t selected_word_value() const;
    [[nodiscard]] bool dirty() const;

    void select_next_word();
    void select_previous_word();
    void adjust_selected_word(std::int32_t delta);

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t selected_word_index_{};
    bool dirty_{};
};

// Free-fly keys held this frame.
struct FlyInput {
    bool forward{};
    bool back{};
    bool left{};
    bool right{};
    bool up{};
    bool down{};
    bool slow{};  // Shift
    bool fast{};  // Z: speeds up, or with Shift slows further
};

// Speed multiplier: Shift 1/4, Z 4x, Shift+Z 1/16.
float fly_speed_multiplier(bool slow, bool fast);

// One frame of fly movement: W/S along the full view direction (pitch
// included), A/D along its level right, Space/Ctrl straight up/down. Combined
// keys are normalized so diagonals are not faster.
Vec3 fly_camera_step(Vec3 forward, const FlyInput& input, float speed, float seconds);

} // namespace map_editor
