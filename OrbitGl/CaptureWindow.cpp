// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "CaptureWindow.h"

#include "App.h"
#include "GlUtils.h"
#include "absl/base/casts.h"

using orbit_client_protos::TimerInfo;

CaptureWindow::CaptureWindow(CaptureWindow::StatsMode stats_mode)
    : GlCanvas(), stats_enabled_(stats_mode == StatsMode::kEnabled) {
  GCurrentTimeGraph = &time_graph_;
  time_graph_.SetTextRenderer(&text_renderer_);
  time_graph_.SetCanvas(this);
  draw_help_ = true;
  draw_filter_ = false;
  first_help_draw_ = true;
  draw_stats_ = false;
  world_top_left_x_ = 0;
  world_top_left_y_ = 0;
  world_max_y_ = 0;

  slider_ = std::make_shared<GlHorizontalSlider>();
  vertical_slider_ = std::make_shared<GlVerticalSlider>();

  slider_->SetDragCallback([&](float ratio) { this->OnDrag(ratio); });
  slider_->SetCanvas(this);

  vertical_slider_->SetDragCallback([&](float ratio) { this->OnVerticalDrag(ratio); });
  vertical_slider_->SetCanvas(this);

  vertical_slider_->SetOrthogonalSliderSize(slider_->GetPixelHeight());
  slider_->SetOrthogonalSliderSize(vertical_slider_->GetPixelHeight());

  GOrbitApp->RegisterCaptureWindow(this);
}

CaptureWindow::~CaptureWindow() {
  if (GCurrentTimeGraph == &time_graph_) GCurrentTimeGraph = nullptr;
}

void CaptureWindow::OnTimer() { GlCanvas::OnTimer(); }

void CaptureWindow::ZoomAll() {
  time_graph_.ZoomAll();
  world_top_left_y_ = world_max_y_;
  ResetHoverTimer();
  NeedsUpdate();
}

void CaptureWindow::UpdateWheelMomentum(float delta_time) {
  GlCanvas::UpdateWheelMomentum(delta_time);

  bool zoom_width = true;  // TODO: !wxGetKeyState(WXK_CONTROL);
  if (zoom_width && wheel_momentum_ != 0.f) {
    time_graph_.ZoomTime(wheel_momentum_, mouse_ratio_);
  }
}

void CaptureWindow::MouseMoved(int x, int y, bool left, bool /*right*/, bool /*middle*/) {
  // TODO: Reduce code duplication, call super!
  float world_x, world_y;
  ScreenToWorld(x, y, world_x, world_y);

  mouse_x_ = world_x;
  mouse_y_ = world_y;
  mouse_pos_x_ = x;
  mouse_pos_y_ = y;

  // Pan
  if (left && !im_gui_active_ && !picking_manager_.IsDragging() && !GOrbitApp->IsCapturing()) {
    float world_min;
    float world_max;

    time_graph_.GetWorldMinMax(world_min, world_max);

    auto width = static_cast<float>(GetWidth());

    world_top_left_x_ = world_click_x_ - static_cast<float>(x) / width * world_width_;
    world_top_left_y_ = world_click_y_ + static_cast<float>(y) / width * world_height_;

    world_top_left_x_ = clamp(world_top_left_x_, world_min, world_max - world_width_);
    world_top_left_y_ =
        clamp(world_top_left_y_, world_height_ - time_graph_.GetThreadTotalHeight(), world_max_y_);

    time_graph_.PanTime(screen_click_x_, x, GetWidth(), static_cast<double>(ref_time_click_));
    UpdateVerticalSlider();
    NeedsUpdate();

    click_was_drag_ = true;
  }

  if (is_selecting_) {
    ScreenToWorld(std::max(x, 0), std::max(y, 0), world_x, world_y);
    select_stop_ = Vec2(world_x, world_y);
    time_stop_ = time_graph_.GetTickFromWorld(world_x);
  }

  if (left) {
    picking_manager_.Drag(x, y);
  }

  ResetHoverTimer();

  NeedsRedraw();
}

void CaptureWindow::LeftDown(int x, int y) {
  // Store world clicked pos for panning
  ScreenToWorld(x, y, world_click_x_, world_click_y_);
  screen_click_x_ = x;
  screen_click_y_ = y;
  ref_time_click_ = static_cast<uint64_t>(time_graph_.GetTime(static_cast<double>(x) / GetWidth()));

  is_selecting_ = false;

  Orbit_ImGui_MouseButtonCallback(this, 0, true);

  picking_ = true;
  click_was_drag_ = false;
  NeedsRedraw();
}

void CaptureWindow::LeftUp() {
  GlCanvas::LeftUp();

  if (!click_was_drag_ && background_clicked_) {
    GOrbitApp->SelectTextBox(nullptr);
    GOrbitApp->set_selected_thread_id(SamplingProfiler::kAllThreadsFakeTid);
    NeedsUpdate();
  }
}

void CaptureWindow::LeftDoubleClick() {
  GlCanvas::LeftDoubleClick();
  double_clicking_ = true;
  picking_ = true;
}

void CaptureWindow::Pick() {
  picking_ = true;
  NeedsRedraw();
}

void CaptureWindow::Pick(int x, int y) {
  // 4 bytes per pixel (RGBA), 1x1 bitmap
  std::array<uint8_t, 4 * 1 * 1> pixels{};
  glReadPixels(x, m_MainWindowHeight - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixels[0]);
  uint32_t value;
  std::memcpy(&value, &pixels[0], sizeof(uint32_t));
  PickingId pick_id = PickingId::FromPixelValue(value);

  Pick(pick_id, x, y);

  NeedsUpdate();
}

void CaptureWindow::Pick(PickingId picking_id, int x, int y) {
  PickingType type = picking_id.type;
  background_clicked_ = false;

  Batcher& batcher = GetBatcherById(picking_id.batcher_id);
  const TextBox* text_box = batcher.GetTextBox(picking_id);
  if (text_box) {
    SelectTextBox(text_box);
  } else if (type == PickingType::kPickable) {
    picking_manager_.Pick(picking_id, x, y);
  } else {
    // If the background is clicked: The selection should only be cleared
    // if the user doesn't drag around the capture window.
    // This is handled later in CaptureWindow::LeftUp()
    background_clicked_ = true;
  }
}

void CaptureWindow::SelectTextBox(const TextBox* text_box) {
  if (text_box == nullptr) return;
  GOrbitApp->SelectTextBox(text_box);
  GOrbitApp->set_selected_thread_id(text_box->GetTimerInfo().thread_id());

  const TimerInfo& timer_info = text_box->GetTimerInfo();

  if (double_clicking_) {
    time_graph_.Zoom(timer_info);
  }
}

void CaptureWindow::Hover(int x, int y) {
  // 4 bytes per pixel (RGBA), 1x1 bitmap
  uint8_t pixels[1 * 1 * 4] = {0, 0, 0, 0};
  glReadPixels(x, m_MainWindowHeight - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixels[0]);

  auto pick_id = absl::bit_cast<PickingId>(pixels);
  Batcher& batcher = GetBatcherById(pick_id.batcher_id);

  std::string tooltip;

  if (pick_id.type == PickingType::kPickable) {
    auto pickable = GetPickingManager().GetPickableFromId(pick_id);
    if (pickable) {
      tooltip = pickable->GetTooltip();
    }
  } else {
    PickingUserData* user_data = batcher.GetUserData(pick_id);

    if (user_data && user_data->generate_tooltip_) {
      tooltip = user_data->generate_tooltip_(pick_id);
    }
  }

  GOrbitApp->SendTooltipToUi(tooltip);
}

void CaptureWindow::PreRender() {
  // TODO: Move to GlCanvas?
  if (is_mouse_over_ && can_hover_ && hover_timer_.QueryMillis() > hover_delay_ms_) {
    is_hovering_ = true;
    picking_ = true;
    NeedsRedraw();
  }

  m_NeedsRedraw = m_NeedsRedraw || time_graph_.IsRedrawNeeded();
}

void CaptureWindow::PostRender() {
  if (is_hovering_) {
    is_hovering_ = false;
    can_hover_ = false;
    picking_ = false;
    hover_timer_.Reset();

    Hover(mouse_pos_x_, mouse_pos_y_);
    NeedsUpdate();
    GlCanvas::Render(width_, height_);
    hover_timer_.Reset();
  }

  if (picking_) {
    picking_ = false;
    Pick(screen_click_x_, screen_click_y_);
    NeedsRedraw();
    GlCanvas::Render(width_, height_);
  }
}

void CaptureWindow::Resize(int width, int height) {
  GlCanvas::Resize(width, height);
  NeedsUpdate();
}

void CaptureWindow::RightDown(int x, int y) {
  ScreenToWorld(x, y, world_click_x_, world_click_y_);
  screen_click_x_ = x;
  screen_click_y_ = y;
  Pick();

  is_selecting_ = true;
  select_start_ = Vec2(world_click_x_, world_click_y_);
  select_stop_ = select_start_;
  time_start_ = time_graph_.GetTickFromWorld(world_click_x_);
  time_stop_ = time_start_;
}

bool CaptureWindow::RightUp() {
  if (is_selecting_ && (select_start_[0] != select_stop_[0]) && ControlPressed()) {
    float min_world = std::min(select_stop_[0], select_start_[0]);
    float max_world = std::max(select_stop_[0], select_start_[0]);

    double new_min = time_graph_.GetTime((min_world - world_top_left_x_) / world_width_);
    double new_max = time_graph_.GetTime((max_world - world_top_left_x_) / world_width_);

    time_graph_.SetMinMax(new_min, new_max);
    select_start_ = select_stop_;
  }

  bool show_context_menu = select_start_[0] == select_stop_[0];
  is_selecting_ = false;
  NeedsRedraw();
  return show_context_menu;
}

void CaptureWindow::MiddleDown(int x, int y) {
  float world_x, world_y;
  ScreenToWorld(x, y, world_x, world_y);
  is_selecting_ = true;
  select_start_ = Vec2(world_x, world_y);
  select_stop_ = select_start_;
}

void CaptureWindow::MiddleUp(int x, int y) {
  float world_x, world_y;
  ScreenToWorld(x, y, world_x, world_y);
  is_selecting_ = false;

  select_stop_ = Vec2(world_x, world_y);

  NeedsRedraw();
}

void CaptureWindow::Zoom(int delta) {
  if (delta == 0) return;

  delta = -delta;
  auto delta_float = static_cast<float>(delta);

  float world_x;
  float world_y;

  ScreenToWorld(mouse_pos_x_, mouse_pos_y_, world_x, world_y);
  mouse_ratio_ = static_cast<double>(mouse_pos_x_) / GetWidth();

  time_graph_.ZoomTime(delta_float, mouse_ratio_);
  wheel_momentum_ = delta_float * wheel_momentum_ < 0 ? 0 : wheel_momentum_ + delta_float;

  NeedsUpdate();
}

void CaptureWindow::Pan(float ratio) {
  double ref_time = time_graph_.GetTime(static_cast<double>(mouse_pos_x_) / GetWidth());
  time_graph_.PanTime(mouse_pos_x_,
                      mouse_pos_x_ + static_cast<int>(ratio * static_cast<float>(GetWidth())),
                      GetWidth(), ref_time);
  NeedsUpdate();
}

void CaptureWindow::MouseWheelMoved(int x, int y, int delta, bool ctrl) {
  if (delta == 0) return;

  // Normalize and invert sign, so that delta < 0 is zoom in.
  const int delta_normalized = delta < 0 ? 1 : -1;
  const auto delta_float = static_cast<float>(delta_normalized);

  if (delta < min_wheel_delta_) min_wheel_delta_ = delta;
  if (delta > max_wheel_delta_) max_wheel_delta_ = delta;

  float worldx;
  float worldy;

  ScreenToWorld(x, y, worldx, worldy);

  bool zoom_width = !ctrl;
  if (zoom_width) {
    mouse_ratio_ = static_cast<double>(x) / GetWidth();
    time_graph_.ZoomTime(delta_float, mouse_ratio_);
    wheel_momentum_ = delta_float * wheel_momentum_ < 0 ? 0 : wheel_momentum_ + delta_float;
  } else {
    float mouse_relative_y_position = static_cast<float>(y) / static_cast<float>(GetHeight());
    time_graph_.VerticalZoom(delta_float, mouse_relative_y_position);
  }

  // Use the original sign of a_Delta here.
  Orbit_ImGui_ScrollCallback(this, -delta);

  can_hover_ = true;

  NeedsUpdate();
}

void CaptureWindow::MouseWheelMovedHorizontally(int /*x*/, int /*y*/, int delta, bool /*ctrl*/) {
  if (delta == 0) return;

  // Normalize and invert sign, so that delta < 0 is left.
  int delta_normalized = delta < 0 ? 1 : -1;

  if (delta_normalized < 0) {
    Pan(0.1f);
  } else {
    Pan(-0.1f);
  }

  // Use the original sign of delta here.
  Orbit_ImGui_ScrollCallback(this, -delta_normalized);
}

void CaptureWindow::KeyPressed(unsigned int key_code, bool ctrl, bool shift, bool alt) {
  UpdateSpecialKeys(ctrl, shift, alt);

  ScopeImguiContext state(im_gui_context_);

  if (!im_gui_active_) {
    switch (key_code) {
      case ' ':
        if (!shift) {
          ZoomAll();
        }
        break;
      case 'A':
        Pan(0.1f);
        break;
      case 'D':
        Pan(-0.1f);
        break;
      case 'W':
        Zoom(1);
        break;
      case 'S':
        Zoom(-1);
        break;
      case 'F':
        draw_filter_ = !draw_filter_;
        break;
      case 'I':
        draw_stats_ = !draw_stats_ && stats_enabled_;
        break;
      case 'H':
        draw_help_ = !draw_help_;
        break;
      case 'X':
        GOrbitApp->ToggleCapture();
        draw_help_ = false;
#ifdef __linux__
        ZoomAll();
#endif
        break;
      case 'O':
        if (ctrl) {
          text_renderer_.ToggleDrawOutline();
        }
        break;
      case 18:  // Left
        if (shift) {
          time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                        TimeGraph::JumpDirection::kPrevious,
                                        TimeGraph::JumpScope::kSameFunction);
        } else if (alt) {
          time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                        TimeGraph::JumpDirection::kPrevious,
                                        TimeGraph::JumpScope::kSameThreadSameFunction);
        } else {
          time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                        TimeGraph::JumpDirection::kPrevious,
                                        TimeGraph::JumpScope::kSameDepth);
        }
        break;
      case 20:  // Right
        if (shift) {
          time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                        TimeGraph::JumpDirection::kNext,
                                        TimeGraph::JumpScope::kSameFunction);
        } else if (alt) {
          time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                        TimeGraph::JumpDirection::kNext,
                                        TimeGraph::JumpScope::kSameThreadSameFunction);
        } else {
          time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                        TimeGraph::JumpDirection::kNext,
                                        TimeGraph::JumpScope::kSameDepth);
        }
        break;
      case 19:  // Up
        time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                      TimeGraph::JumpDirection::kTop,
                                      TimeGraph::JumpScope::kSameThread);
        break;
      case 21:  // Down
        time_graph_.JumpToNeighborBox(GOrbitApp->selected_text_box(),
                                      TimeGraph::JumpDirection::kDown,
                                      TimeGraph::JumpScope::kSameThread);
        break;
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  io.KeyCtrl = ctrl;
  io.KeyShift = shift;
  io.KeyAlt = alt;

  Orbit_ImGui_KeyCallback(this, static_cast<int>(key_code), true);

  NeedsRedraw();
}

std::vector<std::string> CaptureWindow::GetContextMenu() { return std::vector<std::string>{}; }

void CaptureWindow::OnContextMenu(const std::string& /*action*/, int /*menu_index*/) {}

void CaptureWindow::OnCaptureStarted() {
  time_graph_.ZoomAll();
  NeedsRedraw();
}

void CaptureWindow::Draw() {
  world_max_y_ = 1.5f * ScreenToWorldHeight(static_cast<int>(slider_->GetPixelHeight()));

  if (GOrbitApp->IsCapturing()) {
    ZoomAll();
  }

  // Reset picking manager before each draw.
  picking_manager_.Reset();

  time_graph_.Draw(this, GetPickingMode());

  RenderSelectionOverlay();

  if (GetPickingMode() == PickingMode::kNone) {
    RenderTimeBar();

    Vec2 pos(mouse_x_, world_top_left_y_);
    // Vertical green line at mouse x position
    ui_batcher_.AddVerticalLine(pos, -world_height_, kZValueText, Color(0, 255, 0, 127));
  }
}

void CaptureWindow::DrawScreenSpace() {
  double time_span = time_graph_.GetCaptureTimeSpanUs();

  Color col = slider_->GetBarColor();
  auto canvas_height = static_cast<float>(GetHeight());

  const TimeGraphLayout& layout = time_graph_.GetLayout();
  float right_margin = layout.GetRightMargin();

  const auto picking_mode = GetPickingMode();

  if (time_span > 0) {
    double start = time_graph_.GetMinTimeUs();
    double stop = time_graph_.GetMaxTimeUs();
    double width = stop - start;
    double max_start = time_span - width;
    double ratio = GOrbitApp->IsCapturing() ? 1 : (max_start != 0 ? start / max_start : 0);
    float slider_width = layout.GetSliderWidth();
    slider_->SetPixelHeight(slider_width);
    slider_->SetSliderRatio(static_cast<float>(ratio));
    slider_->SetSliderWidthRatio(static_cast<float>(width / time_span));
    slider_->Draw(this, picking_mode);

    float vertical_ratio = world_height_ / time_graph_.GetThreadTotalHeight();
    if (vertical_ratio < 1.f) {
      vertical_slider_->SetPixelHeight(slider_width);
      vertical_slider_->SetSliderWidthRatio(vertical_ratio);
      vertical_slider_->Draw(this, picking_mode);
      right_margin += slider_width;
    }

    vertical_slider_->SetOrthogonalSliderSize(slider_->GetPixelHeight());
    slider_->SetOrthogonalSliderSize(vertical_slider_->GetPixelHeight());
  }

  // Right vertical margin.
  time_graph_.SetRightMargin(right_margin);
  auto margin_x1 = static_cast<float>(GetWidth());
  float margin_x0 = margin_x1 - right_margin;

  Box box(Vec2(margin_x0, 0), Vec2(margin_x1 - margin_x0, canvas_height), GlCanvas::kZValueMargin);
  ui_batcher_.AddBox(box, kBackgroundColor);

  // Time bar background
  if (time_graph_.GetCaptureTimeSpanUs() > 0) {
    Box background_box(Vec2(0, time_graph_.GetLayout().GetSliderWidth()),
                       Vec2(GetWidth(), time_graph_.GetLayout().GetTimeBarHeight()),
                       GlCanvas::kZValueTimeBarBg);
    ui_batcher_.AddBox(background_box,
                       Color(kBackgroundColor[0], kBackgroundColor[1], kBackgroundColor[2], 200));
  }
}

void CaptureWindow::OnDrag(float ratio) {
  time_graph_.OnDrag(ratio);
  NeedsUpdate();
}

void CaptureWindow::OnVerticalDrag(float ratio) {
  float min = world_max_y_;
  float max = world_height_ - time_graph_.GetThreadTotalHeight();
  float range = max - min;
  world_top_left_y_ = min + ratio * range;
  NeedsUpdate();
}

void CaptureWindow::UpdateVerticalSlider() {
  float min = world_max_y_;
  float max = world_height_ - time_graph_.GetThreadTotalHeight();
  float ratio = (world_top_left_y_ - min) / (max - min);
  vertical_slider_->SetSliderRatio(ratio);
}

void CaptureWindow::ToggleDrawHelp() { set_draw_help(!draw_help_); }

void CaptureWindow::set_draw_help(bool draw_help) {
  draw_help_ = draw_help;
  NeedsRedraw();
}

Batcher& CaptureWindow::GetBatcherById(BatcherId batcher_id) {
  switch (batcher_id) {
    case BatcherId::kTimeGraph:
      return time_graph_.GetBatcher();
    case BatcherId::kUi:
      return ui_batcher_;
    default:
      UNREACHABLE();
  }
}

void CaptureWindow::NeedsUpdate() {
  time_graph_.NeedsUpdate();
  m_NeedsRedraw = true;
}

template <class T>
static std::string VariableToString(std::string_view name, const T& value) {
  std::stringstream string_stream{};
  string_stream << name << " = " << value;
  return string_stream.str();
}

void CaptureWindow::RenderUI() {
  // Don't draw ImGui when picking.
  if (GetPickingMode() != PickingMode::kNone) {
    return;
  }

  ScopeImguiContext state(im_gui_context_);
  Orbit_ImGui_NewFrame(this);

#define VAR_TO_STR(var) VariableToString(#var, var)

  if (draw_stats_) {
    ImGui::ShowDemoWindow();
    if (time_graph_.GetLayout().DrawProperties()) {
      NeedsUpdate();
    }

    m_StatsWindow.Clear();

    m_StatsWindow.AddLine(VAR_TO_STR(width_));
    m_StatsWindow.AddLine(VAR_TO_STR(height_));
    m_StatsWindow.AddLine(VAR_TO_STR(world_height_));
    m_StatsWindow.AddLine(VAR_TO_STR(world_width_));
    m_StatsWindow.AddLine(VAR_TO_STR(world_top_left_x_));
    m_StatsWindow.AddLine(VAR_TO_STR(world_top_left_y_));
    m_StatsWindow.AddLine(VAR_TO_STR(world_min_width_));
    m_StatsWindow.AddLine(VAR_TO_STR(mouse_x_));
    m_StatsWindow.AddLine(VAR_TO_STR(mouse_y_));
    m_StatsWindow.AddLine(VAR_TO_STR(time_graph_.GetNumDrawnTextBoxes()));
    m_StatsWindow.AddLine(VAR_TO_STR(time_graph_.GetNumTimers()));
    m_StatsWindow.AddLine(VAR_TO_STR(time_graph_.GetThreadTotalHeight()));

    m_StatsWindow.AddLine(VAR_TO_STR(
        GOrbitApp->GetCaptureData().GetCallstackData()->callstack_events_by_tid().size()));
    m_StatsWindow.AddLine(
        VAR_TO_STR(GOrbitApp->GetCaptureData().GetCallstackData()->GetCallstackEventsCount()));

    m_StatsWindow.Draw("Capture Stats", &draw_stats_);
  }

#undef VAR_TO_STR

  if (draw_help_) {
    RenderHelpUi();

    if (first_help_draw_) {
      // Redraw so that Imgui resizes the
      // window properly on first draw
      NeedsRedraw();
      first_help_draw_ = false;
    }
  }

  // Rendering
  glViewport(0, 0, GetWidth(), GetHeight());
  ImGui::Render();
}

void CaptureWindow::RenderText() {
  if (!picking_) {
    time_graph_.DrawText(this);
  }
}

void ColorToFloat(Color color, float* output) {
  for (size_t i = 0; i < 4; ++i) {
    output[i] = static_cast<float>(color[i]) / 255.f;
  }
}

void CaptureWindow::RenderHelpUi() {
  constexpr float kYOffset = 8.f;
  ImGui::SetNextWindowPos(ImVec2(0, kYOffset));

  ImVec4 color(1.f, 0, 0, 1.f);
  ColorToFloat(slider_->GetBarColor(), &color.x);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, color);

  if (!ImGui::Begin("Help Overlay", &draw_help_, ImVec2(0, 0), 1.f,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PopStyleColor();
    ImGui::End();
    return;
  }

  ImGui::Text("Start/Stop Capture: 'X'");
  ImGui::Text("Pan: 'A','D' or \"Left Click + Drag\"");
  ImGui::Text("Zoom: 'W', 'S', Scroll or \"Ctrl + Right Click + Drag\"");
  ImGui::Text("Select: Left Click");
  ImGui::Text("Measure: \"Right Click + Drag\"");
  ImGui::Text("Toggle Help: 'H'");

  ImGui::End();

  ImGui::PopStyleColor();
}

inline double GetIncrementMs(double milli_seconds) {
  constexpr double kDay = 24 * 60 * 60 * 1000;
  constexpr double kHour = 60 * 60 * 1000;
  constexpr double kMinute = 60 * 1000;
  constexpr double kSecond = 1000;
  constexpr double kMilli = 1;
  constexpr double kMicro = 0.001;
  constexpr double kNano = 0.000001;

  std::string res;

  if (milli_seconds < kMicro) {
    return kNano;
  }
  if (milli_seconds < kMilli) {
    return kMicro;
  }
  if (milli_seconds < kSecond) {
    return kMilli;
  }
  if (milli_seconds < kMinute) {
    return kSecond;
  }
  if (milli_seconds < kHour) {
    return kMinute;
  }
  if (milli_seconds < kDay) {
    return kHour;
  }
  return kDay;
}

void CaptureWindow::RenderTimeBar() {
  static int num_time_points = 10;

  if (time_graph_.GetCaptureTimeSpanUs() > 0) {
    const float time_bar_height = time_graph_.GetLayout().GetTimeBarHeight();

    double millis = time_graph_.GetCurrentTimeSpanUs() * 0.001;
    double incr = millis / float(num_time_points - 1);
    double unit = GetIncrementMs(incr);
    double norm_inc = static_cast<int>((incr + unit) / unit) * unit;
    double start_ms = time_graph_.GetMinTimeUs() * 0.001;
    double norm_start_us = 1000.0 * static_cast<int>(start_ms / norm_inc) * norm_inc;

    static constexpr int kPixelMargin = 2;
    int screen_y = GetHeight() - static_cast<int>(time_bar_height) - kPixelMargin;
    float dummy;
    float world_y;
    ScreenToWorld(0, screen_y, dummy, world_y);

    float height = time_graph_.GetLayout().GetTimeBarHeight() - kPixelMargin;
    float x_margin = ScreenToWorldWidth(4);

    for (int i = 0; i < num_time_points; ++i) {
      double current_micros = norm_start_us + i * 1000 * norm_inc;
      if (current_micros < 0) continue;

      std::string text = GetPrettyTime(absl::Microseconds(current_micros));
      float world_x = time_graph_.GetWorldFromUs(current_micros);
      text_renderer_.AddText(text.c_str(), world_x + x_margin, world_y, GlCanvas::kZValueTextUi,
                             Color(255, 255, 255, 255), GParams.font_size);

      Vec2 pos(world_x, world_y);
      ui_batcher_.AddVerticalLine(pos, height, GlCanvas::kZValueTextUi, Color(255, 255, 255, 255));
    }
  }
}

void CaptureWindow::RenderSelectionOverlay() {
  if (!picking_ && select_start_[0] != select_stop_[0]) {
    uint64_t min_time = std::min(time_start_, time_stop_);
    uint64_t max_time = std::max(time_start_, time_stop_);

    float from = time_graph_.GetWorldFromTick(min_time);
    float to = time_graph_.GetWorldFromTick(max_time);

    float size_x = to - from;
    Vec2 pos(from, world_top_left_y_ - world_height_);
    Vec2 size(size_x, world_height_);

    std::string text = GetPrettyTime(TicksToDuration(min_time, max_time));
    const Color color(0, 128, 0, 128);

    Box box(pos, size, GlCanvas::kZValueOverlay);
    ui_batcher_.AddBox(box, color);

    const Color text_color(255, 255, 255, 255);
    float pos_x = pos[0] + size[0];

    text_renderer_.AddText(text.c_str(), pos_x, select_stop_[1], GlCanvas::kZValueText, text_color,
                           GParams.font_size, size[0], true);

    const unsigned char g = 100;
    Color grey(g, g, g, 255);
    ui_batcher_.AddVerticalLine(pos, size[1], GlCanvas::kZValueOverlay, grey);
  }
}

void CaptureWindow::Initialize() { GlCanvas::Initialize(); }
