#include "lora_history_tree.hpp"
#include "lora_history_lca.hpp"

#include "../service/lora_training_manager.hpp"

#include <rfl/json.hpp>
#include <utf8/cpp20.h>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llavon::settings {
namespace {

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Shapes;
using namespace winrt::Windows::UI::Text;

constexpr double node_diameter = 76;
constexpr double leaf_spacing = 200;
constexpr double level_spacing = 96;
constexpr double graph_padding_x = 340;
constexpr double graph_padding_y = 300;

struct TrainingParameters {
    bool only_manually_selected;
    std::int32_t rank;
    double alpha;
    double dropout;
    std::int32_t batch_size;
    std::int32_t gradient_accumulation;
    std::int32_t epochs;
    std::int32_t max_steps;
    double learning_rate;
    double weight_decay;
    std::int32_t warmup_steps;
    double max_gradient_norm;
    std::int32_t save_every;
    std::int32_t device;
    std::int32_t seed;
    bool shuffle;
    std::int32_t max_sequence_length;
    std::string dtype;
    std::string target_modules;
};

struct Position {
    double x = 0;
    double y = 0;
};

struct PanState {
    bool active = false;
    bool dragging = false;
    bool owns_capture = false;
    bool transferring_capture = false;
    bool pending_view = false;
    bool committing_view = false;
    std::uint32_t pointer_id = 0;
    winrt::Windows::Foundation::Point press{};
    double horizontal_offset = 0;
    double vertical_offset = 0;
    double desired_horizontal = 0;
    double desired_vertical = 0;
    std::optional<winrt::event_token> rendering_token;
};

bool dark_theme() {
    try {
        const auto foreground = winrt::Windows::UI::ViewManagement::UISettings()
            .GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
        return 5u * foreground.G + 2u * foreground.R + foreground.B > 8u * 128u;
    } catch (...) {
        return false;
    }
}

SolidColorBrush brush(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return SolidColorBrush(winrt::Windows::UI::Color{255, red, green, blue});
}

TextBlock text(const std::wstring& value, double size, FontWeight weight,
               const SolidColorBrush& foreground) {
    TextBlock block;
    block.Text(value);
    block.FontFamily(FontFamily(L"Microsoft JhengHei UI"));
    block.FontSize(size);
    block.FontWeight(weight);
    block.Foreground(foreground);
    block.TextWrapping(TextWrapping::Wrap);
    return block;
}

std::wstring widen(std::string_view value) {
    const auto utf16 = utf8::utf8to16(std::string(value));
    return std::wstring(utf16.begin(), utf16.end());
}

std::optional<TrainingParameters> parse_parameters(const LoraHistoryRunView& run) {
    if (run.training_request_json.empty()) return std::nullopt;
    const auto parsed = rfl::json::read<TrainingParameters>(run.training_request_json);
    if (!parsed) return std::nullopt;
    return parsed.value();
}

bool matches_preset(const TrainingParameters& actual,
                    llavon::service::LoraTrainingStrength strength) {
    const llavon::service::LoraTrainingOptions defaults;
    const auto preset = llavon::service::lora_training_preset(strength);
    return std::tie(actual.rank, actual.alpha, actual.dropout,
               actual.batch_size, actual.gradient_accumulation,
               actual.max_steps, actual.weight_decay, actual.warmup_steps,
               actual.max_gradient_norm, actual.save_every, actual.device,
               actual.seed, actual.shuffle, actual.max_sequence_length) ==
           std::tie(defaults.rank, defaults.alpha, defaults.dropout,
               defaults.batch_size, defaults.gradient_accumulation,
               defaults.max_steps, defaults.weight_decay, defaults.warmup_steps,
               defaults.max_gradient_norm, defaults.save_every, defaults.device,
               defaults.seed, defaults.shuffle, defaults.max_sequence_length) &&
           actual.dtype == utf8::utf16to8(defaults.dtype) &&
           actual.target_modules == utf8::utf16to8(defaults.target_modules) &&
           actual.epochs == preset.epochs &&
           actual.learning_rate == preset.learning_rate;
}

const wchar_t* preset_name(const TrainingParameters& parameters) {
    using llavon::service::LoraTrainingStrength;
    if (matches_preset(parameters, LoraTrainingStrength::ultra_low)) return L"極低";
    if (matches_preset(parameters, LoraTrainingStrength::low)) return L"低";
    if (matches_preset(parameters, LoraTrainingStrength::medium)) return L"中";
    if (matches_preset(parameters, LoraTrainingStrength::high)) return L"高";
    return nullptr;
}

std::vector<std::wstring> full_parameter_lines(const TrainingParameters& parameters) {
    const wchar_t* device = parameters.device == 1 ? L"cuda" :
        (parameters.device == 2 ? L"cpu" : L"auto");
    return {
        std::format(L"LoRA rank {} · alpha {:g} · dropout {:g}",
            parameters.rank, parameters.alpha, parameters.dropout),
        std::format(L"Target modules: {}", widen(parameters.target_modules)),
        std::format(L"Batch size {} · gradient accumulation {}",
            parameters.batch_size, parameters.gradient_accumulation),
        std::format(L"Epochs {} · max steps {} · learning rate {:g}",
            parameters.epochs, parameters.max_steps, parameters.learning_rate),
        std::format(L"Weight decay {:g} · warmup steps {} · max gradient norm {:g}",
            parameters.weight_decay, parameters.warmup_steps,
            parameters.max_gradient_norm),
        std::format(L"Save every {} · device {} · dtype {}",
            parameters.save_every, device, widen(parameters.dtype)),
        std::format(L"Seed {} · shuffle {} · max sequence length {}",
            parameters.seed, parameters.shuffle ? L"on" : L"off",
            parameters.max_sequence_length),
    };
}

struct SelectionState {
    std::int64_t selected_id = 0;
    std::int64_t applied_id = 0;
    LoraHistoryTreeAction action = LoraHistoryTreeAction::apply_model;
    bool suppress_node_click = false;
    Button confirm{nullptr};
    std::vector<std::pair<std::int64_t, winrt::weak_ref<Button>>> nodes;
    SolidColorBrush background{nullptr};
    SolidColorBrush normal{nullptr};
    SolidColorBrush selected{nullptr};
    SolidColorBrush applied{nullptr};

    void refresh() const {
        for (const auto& [id, weak_node] : nodes) {
            if (const auto node = weak_node.get()) {
                node.Background(background);
                node.BorderBrush(id == selected_id ? selected :
                    (id == applied_id ? applied : normal));
                const double width = id == selected_id ? 3.0 : 2.0;
                node.BorderThickness(Thickness{width, width, width, width});
            }
        }
        confirm.IsEnabled(true);
    }
};

} // namespace

LoraHistoryTreeView make_lora_history_tree(std::vector<LoraHistoryRunView> runs,
                            std::int64_t selected_id,
                            std::int64_t applied_id,
                            LoraHistoryTreeAction action,
                            std::function<void(std::int64_t)> on_action) {
    const bool dark = dark_theme();
    const auto background = dark ? brush(31, 35, 42) : brush(250, 251, 253);
    const auto text_color = dark ? brush(242, 245, 248) : brush(27, 40, 58);
    const auto muted = dark ? brush(182, 194, 205) : brush(88, 103, 120);
    const auto normal = dark ? brush(137, 166, 195) : brush(129, 151, 174);
    const auto selected = dark ? brush(108, 187, 255) : brush(0, 103, 192);
    const auto applied = brush(32, 153, 108);

    auto selection = std::make_shared<SelectionState>();
    selection->selected_id = selected_id;
    selection->applied_id = applied_id;
    selection->action = action;
    selection->background = background;
    selection->normal = normal;
    selection->selected = selected;
    selection->applied = applied;

    Grid root;
    root.Width(680);
    root.Height(560);
    root.RowDefinitions().Append(RowDefinition{});
    root.RowDefinitions().GetAt(0).Height(GridLength{48, GridUnitType::Pixel});
    root.RowDefinitions().Append(RowDefinition{});
    root.RowDefinitions().GetAt(1).Height(GridLength{1, GridUnitType::Star});
    root.RowDefinitions().Append(RowDefinition{});
    root.RowDefinitions().GetAt(2).Height(GridLength{52, GridUnitType::Pixel});

    StackPanel heading;
    heading.Children().Append(text(action == LoraHistoryTreeAction::apply_model
        ? L"從訓練歷程套用模型" : L"選擇訓練基底", 18,
        FontWeights::SemiBold(), text_color));
    Grid::SetRow(heading, 0);
    root.Children().Append(heading);

    ScrollViewer scroll;
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Visible);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Visible);
    scroll.ZoomMode(ZoomMode::Enabled);
    scroll.MinZoomFactor(0.6f);
    scroll.MaxZoomFactor(2.5f);
    Canvas graph;
    graph.Background(background);
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> children;
    std::unordered_map<std::int64_t, const LoraHistoryRunView*> by_id;
    for (const auto& run : runs) by_id.emplace(run.id, &run);
    for (const auto& run : runs) {
        const auto parent = run.parent_id != 0 && by_id.contains(run.parent_id)
            ? run.parent_id : 0;
        children[parent].push_back(run.id);
    }
    std::unordered_map<std::int64_t, Position> positions;
    std::size_t leaf_count = 0;
    int deepest = 0;
    const std::function<double(std::int64_t, int)> place =
        [&](std::int64_t id, int depth) -> double {
            deepest = std::max(deepest, depth);
            const auto found = children.find(id);
            double x = 0;
            if (found == children.end() || found->second.empty()) {
                x = graph_padding_x + 120 +
                    static_cast<double>(leaf_count++) * leaf_spacing;
            } else {
                const double first = place(found->second.front(), depth + 1);
                double last = first;
                for (std::size_t index = 1; index < found->second.size(); ++index) {
                    last = place(found->second[index], depth + 1);
                }
                x = (first + last) / 2;
            }
            positions.emplace(id, Position{
                x, graph_padding_y + 26 + depth * level_spacing});
            return x;
        };
    place(0, 0);
    graph.Width(std::max(640.0, 240.0 + (leaf_count - 1) * leaf_spacing) +
        2 * graph_padding_x);
    graph.Height(68 + deepest * level_spacing + node_diameter +
        2 * graph_padding_y);

    auto pan = std::make_shared<PanState>();
    const auto weak_graph = winrt::make_weak(graph);
    const auto weak_scroll = winrt::make_weak(scroll);
    const auto update_translation = [pan, weak_graph, weak_scroll] {
        const auto graph = weak_graph.get();
        const auto scroll = weak_scroll.get();
        if (!graph || !scroll) return;
        if (!pan->dragging && !pan->committing_view) return;
        const double zoom = scroll.ZoomFactor();
        graph.Translation(winrt::Windows::Foundation::Numerics::float3{
            static_cast<float>((scroll.HorizontalOffset() -
                pan->desired_horizontal) / zoom),
            static_cast<float>((scroll.VerticalOffset() -
                pan->desired_vertical) / zoom), 0});
    };
    const auto finish_commit = [pan, weak_graph, weak_scroll] {
        if (!pan->committing_view) return;
        const auto graph = weak_graph.get();
        const auto scroll = weak_scroll.get();
        if (!graph || !scroll) return;
        if (std::abs(scroll.HorizontalOffset() - pan->desired_horizontal) >= 0.5 ||
            std::abs(scroll.VerticalOffset() - pan->desired_vertical) >= 0.5) return;
        graph.Translation(winrt::Windows::Foundation::Numerics::float3{});
        pan->committing_view = false;
    };
    const auto apply_pan = [pan, weak_scroll] {
        if (!pan->pending_view) return;
        pan->pending_view = false;
        const auto scroll = weak_scroll.get();
        if (!scroll) return;
        if (std::abs(scroll.HorizontalOffset() - pan->desired_horizontal) < 0.5 &&
            std::abs(scroll.VerticalOffset() - pan->desired_vertical) < 0.5) return;
        scroll.ChangeView(
            winrt::box_value(pan->desired_horizontal).as<
                winrt::Windows::Foundation::IReference<double>>(),
            winrt::box_value(pan->desired_vertical).as<
                winrt::Windows::Foundation::IReference<double>>(),
            nullptr, true);
    };
    const auto stop_rendering = [pan] {
        if (!pan->rendering_token) return;
        CompositionTarget::Rendering(*pan->rendering_token);
        pan->rendering_token.reset();
    };
    scroll.ViewChanged([pan, update_translation, finish_commit](const auto&, const auto&) {
        if (!pan->dragging && !pan->committing_view) return;
        update_translation();
        finish_commit();
    });
    graph.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(PointerEventHandler(
        [pan, selection, weak_graph, weak_scroll](
            const auto&, const PointerRoutedEventArgs& args) {
            const auto graph = weak_graph.get();
            const auto scroll = weak_scroll.get();
            if (!graph || !scroll || pan->active) return;
            const auto point = args.GetCurrentPoint(scroll);
            if (!point.Properties().IsLeftButtonPressed()) return;

            bool pressed_node = false;
            for (auto source = args.OriginalSource().try_as<DependencyObject>();
                 source && source != graph;
                 source = VisualTreeHelper::GetParent(source)) {
                if (source.try_as<Button>()) {
                    pressed_node = true;
                    break;
                }
            }
            pan->active = true;
            pan->dragging = false;
            pan->pending_view = false;
            pan->committing_view = false;
            graph.Translation(winrt::Windows::Foundation::Numerics::float3{});
            pan->pointer_id = args.Pointer().PointerId();
            pan->press = point.Position();
            pan->horizontal_offset = scroll.HorizontalOffset();
            pan->vertical_offset = scroll.VerticalOffset();
            pan->desired_horizontal = pan->horizontal_offset;
            pan->desired_vertical = pan->vertical_offset;
            pan->owns_capture = !pressed_node && graph.CapturePointer(args.Pointer());
            selection->suppress_node_click = false;
        })), true);
    graph.AddHandler(UIElement::PointerMovedEvent(), winrt::box_value(PointerEventHandler(
        [pan, selection, weak_graph, weak_scroll, apply_pan,
         update_translation, finish_commit, stop_rendering](
            const auto&, const PointerRoutedEventArgs& args) {
            if (!pan->active || args.Pointer().PointerId() != pan->pointer_id) return;
            const auto scroll = weak_scroll.get();
            if (!scroll) return;
            const auto point = args.GetCurrentPoint(scroll);
            if (!point.Properties().IsLeftButtonPressed()) return;
            const double dx = point.Position().X - pan->press.X;
            const double dy = point.Position().Y - pan->press.Y;
            bool just_started = false;
            if (!pan->dragging) {
                if (dx * dx + dy * dy < 16 ||
                    (scroll.ScrollableWidth() <= 0.5 &&
                     scroll.ScrollableHeight() <= 0.5)) return;
                pan->dragging = true;
                just_started = true;
                selection->suppress_node_click = true;
                if (!pan->owns_capture) {
                    if (const auto graph = weak_graph.get()) {
                        pan->transferring_capture = true;
                        pan->owns_capture = graph.CapturePointer(args.Pointer());
                        pan->transferring_capture = false;
                    }
                }
                pan->rendering_token = CompositionTarget::Rendering(
                    [apply_pan, update_translation, finish_commit,
                     stop_rendering, weak_graph](const auto&, const auto&) {
                        if (!weak_graph.get()) {
                            stop_rendering();
                            return;
                        }
                        apply_pan();
                        update_translation();
                        finish_commit();
                    });
            }
            pan->desired_horizontal = std::clamp(
                pan->horizontal_offset - dx, 0.0, scroll.ScrollableWidth());
            pan->desired_vertical = std::clamp(
                pan->vertical_offset - dy, 0.0, scroll.ScrollableHeight());
            pan->pending_view = true;
            update_translation();
            if (just_started) apply_pan();
            args.Handled(true);
        })), true);
    const auto end_pan = [pan, selection, weak_graph, apply_pan,
                          update_translation, finish_commit, stop_rendering](
        const PointerRoutedEventArgs& args, bool release_capture) {
        if (!pan->active || args.Pointer().PointerId() != pan->pointer_id) return;
        const bool dragged = pan->dragging;
        const bool owned_capture = pan->owns_capture;
        if (dragged) {
            pan->committing_view = true;
            apply_pan();
            update_translation();
            finish_commit();
        }
        stop_rendering();
        pan->active = false;
        pan->dragging = false;
        pan->owns_capture = false;
        pan->pending_view = false;
        if (const auto graph = weak_graph.get()) {
            if (release_capture && owned_capture)
                graph.ReleasePointerCapture(args.Pointer());
            if (dragged) {
                args.Handled(true);
                if (!graph.DispatcherQueue().TryEnqueue([selection] {
                        selection->suppress_node_click = false;
                    })) selection->suppress_node_click = false;
            }
        }
    };
    graph.AddHandler(UIElement::PointerReleasedEvent(), winrt::box_value(PointerEventHandler(
        [end_pan](const auto&, const PointerRoutedEventArgs& args) {
            end_pan(args, true);
        })), true);
    graph.AddHandler(UIElement::PointerCanceledEvent(), winrt::box_value(PointerEventHandler(
        [end_pan](const auto&, const PointerRoutedEventArgs& args) {
            end_pan(args, true);
        })), true);
    graph.AddHandler(UIElement::PointerCaptureLostEvent(), winrt::box_value(PointerEventHandler(
        [pan, end_pan](const auto&, const PointerRoutedEventArgs& args) {
            if (!pan->transferring_capture) end_pan(args, false);
        })), true);
    graph.Unloaded([pan, stop_rendering](const auto&, const auto&) {
        stop_rendering();
        pan->active = false;
        pan->pending_view = false;
        pan->committing_view = false;
    });
    graph.AddHandler(UIElement::PointerWheelChangedEvent(),
        winrt::box_value(PointerEventHandler(
        [weak_scroll, weak_graph](const auto&, const PointerRoutedEventArgs& args) {
            const auto scroll = weak_scroll.get();
            const auto graph = weak_graph.get();
            if (!scroll || !graph) return;
            const auto point = args.GetCurrentPoint(scroll);
            const auto properties = point.Properties();
            if (properties.IsHorizontalMouseWheel()) return;
            const auto delta = properties.MouseWheelDelta();
            if (delta == 0) return;

            const double old_zoom = scroll.ZoomFactor();
            const double new_zoom = std::clamp(
                old_zoom * std::pow(1.12, static_cast<double>(delta) / 120.0),
                static_cast<double>(scroll.MinZoomFactor()),
                static_cast<double>(scroll.MaxZoomFactor()));
            const auto pointer = point.Position();
            const double horizontal = std::clamp(
                (scroll.HorizontalOffset() + pointer.X) * new_zoom / old_zoom -
                    pointer.X,
                0.0, std::max(0.0, graph.Width() * new_zoom -
                    scroll.ViewportWidth()));
            const double vertical = std::clamp(
                (scroll.VerticalOffset() + pointer.Y) * new_zoom / old_zoom -
                    pointer.Y,
                0.0, std::max(0.0, graph.Height() * new_zoom -
                    scroll.ViewportHeight()));
            scroll.ChangeView(
                winrt::box_value(horizontal).as<
                    winrt::Windows::Foundation::IReference<double>>(),
                winrt::box_value(vertical).as<
                    winrt::Windows::Foundation::IReference<double>>(),
                winrt::box_value(static_cast<float>(new_zoom)).as<
                    winrt::Windows::Foundation::IReference<float>>(), true);
            args.Handled(true);
        })), true);

    const auto add_line = [&](double x1, double y1, double x2, double y2) {
        Line line;
        line.X1(x1);
        line.Y1(y1);
        line.X2(x2);
        line.Y2(y2);
        line.Stroke(normal);
        line.StrokeThickness(2);
        line.IsHitTestVisible(false);
        graph.Children().Append(line);
    };
    for (const auto& run : runs) {
        const auto parent = positions.at(
            run.parent_id != 0 && positions.contains(run.parent_id) ?
                run.parent_id : 0);
        const auto child = positions.at(run.id);
        const double middle = (parent.y + node_diameter + child.y) / 2;
        add_line(parent.x, parent.y + node_diameter, parent.x, middle);
        add_line(parent.x, middle, child.x, middle);
        add_line(child.x, middle, child.x, child.y);
    }

    for (const auto& [id, position] : positions) {
        Button node;
        node.Tag(winrt::box_value(id));
        Automation::AutomationProperties::SetName(node, id == 0
            ? L"Base model" : std::format(L"訓練 #{}", id));
        node.Width(node_diameter);
        node.Height(node_diameter);
        node.MinWidth(node_diameter);
        node.MinHeight(node_diameter);
        node.Padding(Thickness{4, 4, 4, 4});
        node.CornerRadius(CornerRadius{node_diameter / 2, node_diameter / 2,
            node_diameter / 2, node_diameter / 2});
        node.HorizontalContentAlignment(HorizontalAlignment::Center);
        node.VerticalContentAlignment(VerticalAlignment::Center);
        StackPanel face;
        face.Spacing(0);
        const auto add_face_line = [&](const std::wstring& value, bool bold) {
            auto block = text(value, 10, bold ? FontWeights::SemiBold() :
                FontWeights::Normal(), text_color);
            block.HorizontalAlignment(HorizontalAlignment::Center);
            face.Children().Append(block);
        };
        ToolTip tooltip;
        StackPanel details;
        details.Spacing(3);
        details.Width(440);
        if (id == 0) {
            add_face_line(L"Base", true);
            add_face_line(L"model", false);
            details.Children().Append(text(L"Base model", 15,
                FontWeights::SemiBold(), text_color));
            details.Children().Append(text(L"原始模型 · 尚未個人化", 12,
                FontWeights::Normal(), muted));
            if (id == applied_id) {
                details.Children().Append(text(L"目前套用", 12,
                    FontWeights::SemiBold(), applied));
            }
        } else {
            const auto& run = *by_id.at(id);
            const auto space = run.completed_local.find(L' ');
            add_face_line(run.completed_local.substr(0, space), true);
            if (space != std::wstring::npos)
                add_face_line(run.completed_local.substr(space + 1), false);
            details.Children().Append(text(std::format(L"訓練 #{} · {}",
                id, run.completed_local), 15, FontWeights::SemiBold(), text_color));
            details.Children().Append(text(std::format(L"新增 {} 筆 · 累計 {} 筆",
                run.record_count, run.cumulative_record_count), 12,
                FontWeights::Normal(), muted));
            details.Children().Append(text(std::format(L"{} steps · 基底 {}",
                run.optimizer_steps, run.parent_id == 0 ? L"Base" :
                std::format(L"#{}", run.parent_id)), 12,
                FontWeights::Normal(), muted));
            if (const auto parameters = parse_parameters(run)) {
                details.Children().Append(text(parameters->only_manually_selected
                    ? L"資料範圍：只訓練曾手動選字的句子" :
                      L"資料範圍：所有句子", 12,
                    FontWeights::Normal(), muted));
                if (const auto* preset = preset_name(*parameters)) {
                    details.Children().Append(text(std::format(L"訓練強度：{}", preset),
                        12, FontWeights::SemiBold(), selected));
                } else {
                    details.Children().Append(text(L"訓練參數", 12,
                        FontWeights::SemiBold(), text_color));
                    for (const auto& line : full_parameter_lines(*parameters)) {
                        details.Children().Append(text(line, 11,
                            FontWeights::Normal(), muted));
                    }
                }
            } else {
                details.Children().Append(text(L"此歷史紀錄未保存完整訓練參數", 12,
                    FontWeights::Normal(), muted));
                details.Children().Append(text(std::format(
                    L"已知 LoRA rank {} · alpha {:g} · dropout {:g}",
                    run.rank, run.alpha, run.dropout), 11,
                    FontWeights::Normal(), muted));
                details.Children().Append(text(L"Target modules: " +
                    std::wstring(run.target_modules.begin(),
                                 run.target_modules.end()), 11,
                    FontWeights::Normal(), muted));
            }
            if (id == applied_id) {
                details.Children().Append(text(L"目前套用", 12,
                    FontWeights::SemiBold(), applied));
            }
            if (!runs.empty() && id == runs.back().id) {
                details.Children().Append(text(L"最新訓練", 12,
                    FontWeights::SemiBold(), selected));
            }
        }
        node.Content(face);
        tooltip.Content(details);
        const auto hover_timer = node.DispatcherQueue().CreateTimer();
        hover_timer.Interval(std::chrono::milliseconds(120));
        hover_timer.IsRepeating(false);
        const auto weak_node = winrt::make_weak(node);
        hover_timer.Tick([weak_node, tooltip, pan](const auto&, const auto&) {
            if (pan->active) return;
            if (const auto node = weak_node.get()) {
                ToolTipService::SetToolTip(node, tooltip);
                tooltip.IsOpen(true);
            }
        });
        node.PointerEntered([hover_timer, pan](const auto&, const auto&) {
            if (!pan->active) hover_timer.Start();
        });
        const auto close_tooltip = [hover_timer, weak_node, tooltip] {
            hover_timer.Stop();
            tooltip.IsOpen(false);
            if (const auto node = weak_node.get())
                ToolTipService::SetToolTip(node, nullptr);
        };
        node.PointerExited([close_tooltip](const auto&, const auto&) {
            close_tooltip();
        });
        node.PointerPressed([close_tooltip](const auto&, const auto&) {
            close_tooltip();
        });
        node.Unloaded([close_tooltip](const auto&, const auto&) {
            close_tooltip();
        });
        node.Click([selection, id](const auto&, const auto&) {
            if (selection->suppress_node_click) return;
            selection->selected_id = id;
            selection->refresh();
        });
        Canvas::SetLeft(node, position.x - node_diameter / 2);
        Canvas::SetTop(node, position.y);
        graph.Children().Append(node);
        selection->nodes.emplace_back(id, winrt::make_weak(node));
    }
    scroll.Content(graph);
    {
        const auto found = positions.find(selected_id);
        const auto position = found != positions.end() ?
            found->second : positions.at(0);
        auto layout_token = std::make_shared<winrt::event_token>();
        *layout_token = scroll.LayoutUpdated(
            [weak_scroll, position, layout_token](const auto&, const auto&) {
                const auto scroll = weak_scroll.get();
                if (!scroll) return;
                if (scroll.ViewportWidth() <= 0 || scroll.ViewportHeight() <= 0 ||
                    scroll.ScrollableWidth() <= 0 ||
                    scroll.ScrollableHeight() <= 0) return;
                scroll.LayoutUpdated(*layout_token);
                const double horizontal = std::clamp(
                    position.x - scroll.ViewportWidth() / 2,
                    0.0, scroll.ScrollableWidth());
                const double vertical = std::clamp(
                    position.y + node_diameter / 2 - scroll.ViewportHeight() / 2,
                    0.0, scroll.ScrollableHeight());
                scroll.ChangeView(
                    winrt::box_value(horizontal).as<
                        winrt::Windows::Foundation::IReference<double>>(),
                    winrt::box_value(vertical).as<
                        winrt::Windows::Foundation::IReference<double>>(),
                    nullptr, true);
            });
    }
    Border frame;
    frame.CornerRadius(CornerRadius{12, 12, 12, 12});
    frame.BorderBrush(normal);
    frame.BorderThickness(Thickness{1, 1, 1, 1});
    frame.Child(scroll);
    Grid::SetRow(frame, 1);
    root.Children().Append(frame);

    Grid footer;
    footer.Margin(Thickness{0, 12, 0, 0});
    footer.ColumnDefinitions().Append(ColumnDefinition{});
    footer.ColumnDefinitions().GetAt(0).Width(GridLength{1, GridUnitType::Star});
    footer.ColumnDefinitions().Append(ColumnDefinition{});
    footer.ColumnDefinitions().GetAt(1).Width(GridLength{1, GridUnitType::Auto});
    if (runs.size() >= 2) {
        std::vector<LoraHistoryParent> lineage;
        lineage.reserve(runs.size());
        for (const auto& run : runs) {
            lineage.push_back({run.id, run.parent_id});
        }
        const auto latest_id = runs.back().id;
        const auto previous_id = runs[runs.size() - 2].id;
        const auto common_id = tarjan_lca(lineage, latest_id, previous_id);
        if (common_id && *common_id != latest_id && *common_id != previous_id) {
            Button tarjan_button;
            tarjan_button.Content(winrt::box_value(L"Tarjan"));
            tarjan_button.MinWidth(88);
            tarjan_button.HorizontalAlignment(HorizontalAlignment::Left);
            Automation::AutomationProperties::SetName(
                tarjan_button, L"選擇最新與次新訓練的共同祖先");
            tarjan_button.Click([weak = std::weak_ptr<SelectionState>(selection),
                                 id = *common_id](const auto&, const auto&) {
                if (const auto current = weak.lock()) {
                    current->selected_id = id;
                    current->refresh();
                }
            });
            Grid::SetColumn(tarjan_button, 0);
            footer.Children().Append(tarjan_button);
        }
    }
    selection->confirm = Button();
    selection->confirm.Content(winrt::box_value(
        action == LoraHistoryTreeAction::apply_model ?
            L"準備此版本" : L"以此版本為基底"));
    selection->confirm.MinWidth(150);
    selection->confirm.Click([weak = std::weak_ptr<SelectionState>(selection),
                              callback = std::move(on_action)](const auto&, const auto&) {
        if (const auto current = weak.lock()) callback(current->selected_id);
    });
    Grid::SetColumn(selection->confirm, 1);
    footer.Children().Append(selection->confirm);
    Grid::SetRow(footer, 2);
    root.Children().Append(footer);
    selection->refresh();
    return LoraHistoryTreeView{root, selection->confirm, scroll};
}

} // namespace llavon::settings
