/**
 * Wayfire GNOME Shell Plugin (compact version)
 *  copyright andrew pliatsikas
 * A GNOME Shell-like experience for Wayfire:
 * - Top panel with Activities button, clock
 * - Click Activities (or press Super) to enter overview mode
 * - GNOME-like horizontal workspace carousel: adjacent workspaces visible as
 *   large previews to the left/right of the focused workspace
 * - Click on an adjacent large preview to navigate (slides carousel)
 * - Windows animate smoothly to a grid layout using view transformers
 * - Click a window to focus it and exit overview
 * - Drag a window to a workspace (large preview OR small thumbnail) to move it
 * - App icon overlay at bottom of each window (on the window, not below)
 *
 * Copyright (c) 2025
 * Licensed under MIT
 */

#include <cairo.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <vector>
#include <wayfire/core.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/region.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/view.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/workspace-stream.hpp>
#include <sys/stat.h>

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

namespace wf {
namespace overview {

static const std::string TRANSFORMER_NAME = "wayfire-overview";

// ============================================================================
// Simple Animation Helper
// ============================================================================

class anim_t {
  float val, start, goal, duration_ms;
  std::chrono::steady_clock::time_point start_time;
  bool animating = false;
public:
  anim_t(float v = 0) : val(v), start(v), goal(v), duration_ms(300) {}
  void set_duration(float ms) { duration_ms = ms; }
  void animate_to(float g) {
    start = val; goal = g;
    start_time = std::chrono::steady_clock::now();
    animating = true;
  }
  void warp(float v) { val = start = goal = v; animating = false; }
  bool tick() {
    if (!animating) return false;
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float, std::milli>(now - start_time).count();
    float t = std::clamp(elapsed / duration_ms, 0.0f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    val = start + (goal - start) * ease;
    if (t >= 1.0f) { val = goal; animating = false; }
    return animating;
  }
  float value() const { return val; }
  bool is_animating() const { return animating; }
};

struct anim_geo_t {
  anim_t x, y, w, h;
  void set_duration(float ms) { x.set_duration(ms); y.set_duration(ms); w.set_duration(ms); h.set_duration(ms); }
  void animate_to(wf::geometry_t g) { x.animate_to(g.x); y.animate_to(g.y); w.animate_to(g.width); h.animate_to(g.height); }
  void warp(wf::geometry_t g) { x.warp(g.x); y.warp(g.y); w.warp(g.width); h.warp(g.height); }
  bool tick() { bool a = x.tick(), b = y.tick(), c = w.tick(), d = h.tick(); return a || b || c || d; }
  wf::geometry_t current() const { return {(int)x.value(), (int)y.value(), (int)w.value(), (int)h.value()}; }
  bool is_animating() const { return x.is_animating() || y.is_animating() || w.is_animating() || h.is_animating(); }
};

// ============================================================================
// Icon Texture
// ============================================================================

static bool file_exists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

static std::string icon_from_desktop_file(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open()) return "";
  std::string line;
  bool in_entry = false;
  while (std::getline(f, line)) {
    if (line.find("[Desktop Entry]") != std::string::npos) { in_entry = true; continue; }
    if (line.size() > 0 && line[0] == '[') { if (in_entry) break; continue; }
    if (in_entry && line.compare(0, 5, "Icon=") == 0)
      return line.substr(5);
  }
  return "";
}

static std::string find_icon_name_for_app(const std::string &app_id) {
  std::vector<std::string> dirs = {"/usr/share/applications", "/usr/local/share/applications"};
  const char *home = getenv("HOME");
  if (home) dirs.push_back(std::string(home) + "/.local/share/applications");
  dirs.push_back("/var/lib/flatpak/exports/share/applications");
  if (home) dirs.push_back(std::string(home) + "/.local/share/flatpak/exports/share/applications");

  for (auto &dir : dirs) {
    std::string path = dir + "/" + app_id + ".desktop";
    auto icon = icon_from_desktop_file(path);
    if (!icon.empty()) return icon;
  }

  std::string lower_id = app_id;
  std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(), ::tolower);

  for (auto &dir : dirs) {
    DIR *d = opendir(dir.c_str());
    if (!d) continue;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
      std::string name = ent->d_name;
      if (name.size() < 9 || name.substr(name.size()-8) != ".desktop") continue;
      std::string lower_name = name;
      std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
      if (lower_name.find(lower_id) != std::string::npos) {
        auto icon = icon_from_desktop_file(dir + "/" + name);
        closedir(d);
        if (!icon.empty()) return icon;
      }
    }
    closedir(d);
  }
  return "";
}

struct icon_tex_t {
  GLuint tex_id = 0;
  int width = 0, height = 0;

  void upload_surface(cairo_surface_t *src, int target_size) {
    int sw = cairo_image_surface_get_width(src);
    int sh = cairo_image_surface_get_height(src);
    width = height = target_size;
    auto scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    auto cr = cairo_create(scaled);
    double sc = (double)target_size / std::max(sw, sh);
    double ox = (target_size - sw * sc) / 2.0;
    double oy = (target_size - sh * sc) / 2.0;
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(scaled);
    auto data = cairo_image_surface_get_data(scaled);
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    cairo_surface_destroy(scaled);
  }

  bool try_load_png(const std::string &path, int target_size) {
    if (!file_exists(path)) return false;
    auto surf = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(surf); return false; }
    upload_surface(surf, target_size);
    cairo_surface_destroy(surf);
    return true;
  }

  bool load_icon_by_name(const std::string &icon_name, int target_size) {
    if (icon_name.empty()) return false;
    if (icon_name[0] == '/') return try_load_png(icon_name, target_size);
    static const char *sizes[] = {"256x256","128x128","96x96","64x64","48x48","32x32","scalable"};
    static const char *themes[] = {"hicolor","Adwaita","breeze","gnome","Papirus"};
    static const char *bases[] = {"/usr/share/icons", "/usr/local/share/icons"};
    for (auto base : bases) for (auto theme : themes) for (auto sz : sizes) {
      std::string cat = (std::string(sz) == "scalable") ? "scalable" : sz;
      std::string path = std::string(base) + "/" + theme + "/" + cat + "/apps/" + icon_name + ".png";
      if (try_load_png(path, target_size)) return true;
      for (auto sub : {"apps", "mimetypes", "categories", "places"}) {
        path = std::string(base) + "/" + theme + "/" + cat + "/" + sub + "/" + icon_name + ".png";
        if (try_load_png(path, target_size)) return true;
      }
    }
    std::string pixmap = "/usr/share/pixmaps/" + icon_name + ".png";
    return try_load_png(pixmap, target_size);
  }

  bool load_for_app(const std::string &app_id, int target_size) {
    if (load_icon_by_name(app_id, target_size)) return true;
    std::string lower = app_id;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower != app_id && load_icon_by_name(lower, target_size)) return true;
    std::string icon_name = find_icon_name_for_app(app_id);
    if (!icon_name.empty() && load_icon_by_name(icon_name, target_size)) return true;
    std::string alt = app_id;
    for (auto &c : alt) { if (c == '.') c = '-'; }
    if (alt != app_id && load_icon_by_name(alt, target_size)) return true;
    alt = app_id; for (auto &c : alt) { if (c == '-') c = '.'; }
    if (alt != app_id && load_icon_by_name(alt, target_size)) return true;
    return false;
  }

  void create_fallback(const std::string &app_name, int target_size) {
    width = height = target_size;
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    auto cr = cairo_create(surface);
    double r = target_size * 0.22, inset = target_size * 0.04, iw = target_size - inset * 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, inset + r, inset + r, r, M_PI, 1.5 * M_PI);
    cairo_arc(cr, inset + iw - r, inset + r, r, -0.5 * M_PI, 0);
    cairo_arc(cr, inset + iw - r, inset + iw - r, r, 0, 0.5 * M_PI);
    cairo_arc(cr, inset + r, inset + iw - r, r, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);
    auto pat = cairo_pattern_create_linear(0, inset, 0, inset + iw);
    cairo_pattern_add_color_stop_rgba(pat, 0, 0.35, 0.45, 0.55, 0.92);
    cairo_pattern_add_color_stop_rgba(pat, 1, 0.20, 0.30, 0.40, 0.92);
    cairo_set_source(cr, pat); cairo_fill_preserve(cr); cairo_pattern_destroy(pat);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.15); cairo_set_line_width(cr, 1.0); cairo_stroke(cr);
    if (!app_name.empty()) {
      char letter[2] = {(char)toupper(app_name[0]), 0};
      auto layout = pango_cairo_create_layout(cr);
      char font[32]; snprintf(font, sizeof(font), "Sans Bold %d", target_size * 2 / 5);
      auto fd = pango_font_description_from_string(font);
      pango_layout_set_font_description(layout, fd);
      pango_layout_set_text(layout, letter, 1);
      int tw, th; pango_layout_get_pixel_size(layout, &tw, &th);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
      cairo_move_to(cr, (width - tw) / 2.0, (height - th) / 2.0);
      pango_cairo_show_layout(cr, layout);
      pango_font_description_free(fd); g_object_unref(layout);
    }
    cairo_surface_flush(surface);
    auto data = cairo_image_surface_get_data(surface);
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    cairo_destroy(cr); cairo_surface_destroy(surface);
  }

  void destroy() { if (tex_id) { glDeleteTextures(1, &tex_id); tex_id = 0; } width = height = 0; }
};

// ============================================================================
// Window Slot (unchanged from original)
// ============================================================================

struct window_slot_t {
  wayfire_toplevel_view view;
  wf::geometry_t orig_geo, target_geo;
  anim_geo_t anim;
  std::shared_ptr<wf::scene::view_2d_transformer_t> transformer;
  bool hovered = false;
  icon_tex_t icon;
  std::string app_id, app_name;

  void start_anim(bool entering, float duration) {
    anim.set_duration(duration);
    if (entering) { anim.warp(orig_geo); anim.animate_to(target_geo); }
    else { anim.animate_to(orig_geo); }
  }
  void update_transformer() {
    if (!transformer || !view || !view->is_mapped() || orig_geo.width <= 0 || orig_geo.height <= 0) return;
    auto cur = anim.current();
    float sx = std::clamp((float)cur.width / orig_geo.width, 0.1f, 10.0f);
    float sy = std::clamp((float)cur.height / orig_geo.height, 0.1f, 10.0f);
    transformer->translation_x = (cur.x + cur.width/2.0f) - (orig_geo.x + orig_geo.width/2.0f);
    transformer->translation_y = (cur.y + cur.height/2.0f) - (orig_geo.y + orig_geo.height/2.0f);
    transformer->scale_x = sx; transformer->scale_y = sy;
    transformer->alpha = hovered ? 1.0f : 0.88f;
  }
  void reset_transformer() {
    if (!transformer) return;
    transformer->translation_x = transformer->translation_y = 0;
    transformer->scale_x = transformer->scale_y = transformer->alpha = 1.0f;
  }
  static std::string make_app_name(wayfire_toplevel_view v) {
    if (!v) return "";
    auto id = v->get_app_id();
    if (id.empty()) { auto t = v->get_title(); auto sp = t.find(' '); id = (sp != std::string::npos) ? t.substr(0, sp) : t; }
    if (!id.empty()) { id[0] = std::toupper(id[0]); for (auto &c : id) { if (c == '-' || c == '_') c = ' '; } }
    if (id.length() > 24) id = id.substr(0, 22) + "...";
    return id;
  }
};

// ============================================================================
// Top Panel (unchanged from original)
// ============================================================================

class top_panel_t {
public:
  wf::output_t *output;
  cairo_surface_t *surface = nullptr; cairo_t *cr = nullptr;
  GLuint tex_id = 0; int width = 0, height = 16;
  wf::geometry_t activities_bounds{}; bool activities_hovered = false;
  std::string color = "#1a1a1aE6";

  top_panel_t(wf::output_t *out, int h, const std::string &c) : output(out), height(h), color(c) { create(); }
  ~top_panel_t() { destroy(); }

  void create() {
    width = output->get_layout_geometry().width;
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cr = cairo_create(surface); render(); upload();
  }
  void destroy() {
    if (tex_id) { wf::gles::run_in_context([&] { glDeleteTextures(1, &tex_id); }); tex_id = 0; }
    if (cr) { cairo_destroy(cr); cr = nullptr; }
    if (surface) { cairo_surface_destroy(surface); surface = nullptr; }
  }
  void render() {
    if (!cr) return;
    float r = 0.1f, g = 0.1f, b = 0.1f, a = 0.9f;
    if (color.length() >= 7 && color[0] == '#') {
      r = std::stoi(color.substr(1,2), nullptr, 16) / 255.0f;
      g = std::stoi(color.substr(3,2), nullptr, 16) / 255.0f;
      b = std::stoi(color.substr(5,2), nullptr, 16) / 255.0f;
      if (color.length() >= 9) a = std::stoi(color.substr(7,2), nullptr, 16) / 255.0f;
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr, r, g, b, a); cairo_paint(cr);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    int fs = height >= 24 ? 11 : 8; char font[32]; snprintf(font, sizeof(font), "Sans Bold %d", fs);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, "Activities", -1);
    int tw, th; pango_layout_get_pixel_size(layout, &tw, &th);
    int ax = 8, ay = (height - th) / 2;
    activities_bounds = {ax - 4, 0, tw + 8, height};
    if (activities_hovered) { cairo_set_source_rgba(cr, 1, 1, 1, 0.15); cairo_rectangle(cr, activities_bounds.x, 0, activities_bounds.width, height); cairo_fill(cr); }
    cairo_set_source_rgba(cr, 1, 1, 1, 1); cairo_move_to(cr, ax, ay); pango_cairo_show_layout(cr, layout);
    time_t now = time(nullptr); char ts[64]; strftime(ts, sizeof(ts), "%a %b %d  %H:%M", localtime(&now));
    pango_layout_set_text(layout, ts, -1); pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, (width - tw) / 2, (height - th) / 2); pango_cairo_show_layout(cr, layout);
    pango_font_description_free(fd); g_object_unref(layout); cairo_surface_flush(surface);
  }
  void upload() {
    if (!surface) return; unsigned char *data = cairo_image_surface_get_data(surface);
    wf::gles::run_in_context([&] {
      if (!tex_id) glGenTextures(1, &tex_id);
      glBindTexture(GL_TEXTURE_2D, tex_id);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
      glBindTexture(GL_TEXTURE_2D, 0);
    });
  }
  wf::geometry_t get_geometry() const { auto og = output->get_layout_geometry(); return {og.x, og.y, width, height}; }
  wf::geometry_t get_render_geometry() const { auto og = output->get_layout_geometry(); return {og.x, og.y + og.height - height, width, height}; }
  bool set_hover(bool h) { if (activities_hovered == h) return false; activities_hovered = h; render(); upload(); return true; }
  bool point_in_activities(wf::pointf_t p) const {
    auto og = output->get_layout_geometry(); int lx = p.x - og.x, ly = p.y - og.y;
    return lx >= activities_bounds.x && lx < activities_bounds.x + activities_bounds.width && ly >= 0 && ly < height;
  }
};

// ============================================================================
// Drag State
// ============================================================================

struct drag_state_t {
  bool active = false; wayfire_toplevel_view view = nullptr; int slot_index = -1;
  wf::pointf_t grab_cursor{0, 0}, current_cursor{0, 0};
  wf::geometry_t initial_screen_geo{}; int hover_ws = -1; int hover_large_ws = -1;
  wf::auxilliary_buffer_t snapshot_fb; bool has_snapshot = false, needs_capture = false;
  int float_width = 0, float_height = 0; wf::geometry_t view_geo{};
  wf::pointf_t grab_offset_in_window{0, 0};
  void reset() { active = false; view = nullptr; slot_index = -1; hover_ws = -1; hover_large_ws = -1; has_snapshot = false; needs_capture = false; }
};

// ============================================================================
// Activities View — uses EXACT same coordinate system as original
// ============================================================================

class activities_view_t {
public:
  wf::output_t *output;

  struct ws_window_data_t {
    std::vector<window_slot_t> slots;
    bool transformers_attached = false;
  };
  std::vector<ws_window_data_t> ws_windows;

  int focused_ws = 0;
  wf::point_t orig_ws{0, 0};
  wayfire_toplevel_view hovered_view = nullptr;
  anim_t carousel_scroll;  // pixel offset in render-space X

  // Preview dimensions — computed IDENTICALLY to original
  wf::geometry_t preview_geo{};  // base preview rect (for focused ws)
  int carousel_gap = 0;

  std::vector<wf::geometry_t> ws_geos;
  int ws_rows = 1, ws_cols = 1, total_ws = 1;
  wf::point_t cur_ws{0, 0};
  anim_geo_t desktop_anim;

  bool is_active = false, is_animating = false;
  bool switching_ws = false; int pending_ws = -1;
  int corner_radius = 12, spacing = 20, panel_height = 16, anim_duration = 300;
  int icon_size = 72;
  drag_state_t drag;

  activities_view_t(wf::output_t *out) : output(out) { desktop_anim.set_duration(300); carousel_scroll.set_duration(300); }
  ~activities_view_t() { cleanup_all(); }

  void set_config(int cr, int sp, int ph, int ad) {
    corner_radius = cr; spacing = sp; panel_height = ph; anim_duration = ad;
    desktop_anim.set_duration(ad); carousel_scroll.set_duration(ad);
  }

  void toggle() { is_active ? deactivate() : activate(); }

  std::vector<wayfire_toplevel_view> get_views_on_workspace(wf::point_t ws) {
    auto cur = output->wset()->get_current_workspace();
    auto og = output->get_layout_geometry();
    std::vector<wayfire_toplevel_view> result;
    for (auto &v : output->wset()->get_views(wf::WSET_MAPPED_ONLY)) {
      auto tv = wf::toplevel_cast(v);
      if (!tv || tv->get_output() != output || tv->minimized || !tv->is_mapped()) continue;
      auto vg = tv->get_geometry();
      int vws_x = cur.x + (int)std::floor((float)(vg.x + vg.width/2) / og.width);
      int vws_y = cur.y + (int)std::floor((float)(vg.y + vg.height/2) / og.height);
      if (vws_x == ws.x && vws_y == ws.y) result.push_back(tv);
    }
    return result;
  }

  wf::point_t ws_index_to_point(int idx) const { return {idx % ws_cols, idx / ws_cols}; }
  int ws_point_to_index(wf::point_t p) const { return p.y * ws_cols + p.x; }

  void activate() {
    if (is_active) return;
    is_active = is_animating = true; drag.reset();

    auto wsize = output->wset()->get_workspace_grid_size();
    ws_cols = wsize.width; ws_rows = wsize.height; total_ws = ws_cols * ws_rows;
    cur_ws = output->wset()->get_current_workspace();
    orig_ws = cur_ws;
    focused_ws = ws_point_to_index(cur_ws);

    ws_windows.clear(); ws_windows.resize(total_ws);
    auto og = output->get_layout_geometry();

    for (int i = 0; i < total_ws; i++) {
      auto wsp = ws_index_to_point(i);
      int ox = (wsp.x - cur_ws.x) * og.width, oy = (wsp.y - cur_ws.y) * og.height;
      for (auto &tv : get_views_on_workspace(wsp)) {
        window_slot_t s; s.view = tv;
        auto vg = tv->get_geometry();
        s.orig_geo = {vg.x - ox, vg.y - oy, vg.width, vg.height};
        if (s.orig_geo.width <= 0) s.orig_geo.width = 100;
        if (s.orig_geo.height <= 0) s.orig_geo.height = 100;
        s.app_id = tv->get_app_id(); s.app_name = window_slot_t::make_app_name(tv);
        ws_windows[i].slots.push_back(s);
      }
    }

    arrange();
    for (int i = 0; i < total_ws; i++) { attach_transformers_for_ws(i); load_icons_for_ws(i); }

    carousel_scroll.set_duration(anim_duration);
    carousel_scroll.warp(scroll_target_for(focused_ws));

    desktop_anim.warp({0, 0, og.width, og.height});
    desktop_anim.animate_to(preview_geo);
  }

  void load_icons_for_ws(int wi) {
    if (wi < 0 || wi >= (int)ws_windows.size()) return;
    wf::gles::run_in_context([&] {
      for (auto &s : ws_windows[wi].slots)
        if (!s.icon.load_for_app(s.app_id, icon_size)) s.icon.create_fallback(s.app_name, icon_size);
    });
  }

  void attach_transformers_for_ws(int wi) {
    if (wi < 0 || wi >= (int)ws_windows.size()) return;
    auto &wd = ws_windows[wi]; if (wd.transformers_attached) return;
    for (auto &s : wd.slots) {
      if (!s.view || !s.view->is_mapped()) continue;
      s.transformer = std::make_shared<wf::scene::view_2d_transformer_t>(s.view);
      s.view->get_transformed_node()->add_transformer(s.transformer, wf::TRANSFORMER_2D, TRANSFORMER_NAME);
      s.start_anim(true, anim_duration);
    }
    wd.transformers_attached = true;
  }

  void detach_transformers_for_ws(int wi) {
    if (wi < 0 || wi >= (int)ws_windows.size()) return;
    auto &wd = ws_windows[wi];
    for (auto &s : wd.slots) {
      if (s.view && s.view->is_mapped() && s.transformer) { s.reset_transformer(); s.view->get_transformed_node()->rem_transformer(TRANSFORMER_NAME); }
      s.transformer = nullptr;
    }
    wd.transformers_attached = false;
  }

  void deactivate() {
    if (!is_active) return; drag.reset(); is_animating = true;
    for (auto &wd : ws_windows) for (auto &s : wd.slots) s.start_anim(false, anim_duration);
    if (focused_ws != ws_point_to_index(orig_ws)) { pending_ws = focused_ws; switching_ws = true; }
    auto og = output->get_layout_geometry();
    desktop_anim.animate_to({0, 0, og.width, og.height});
  }

  void deactivate_to_ws(int idx) {
    if (!is_active) return; drag.reset();
    pending_ws = idx; switching_ws = true;
    // Animate windows back to original positions
    for (auto &wd : ws_windows) for (auto &s : wd.slots) s.start_anim(false, anim_duration);
    // Warp desktop_anim to the small thumbnail position, then zoom to full screen
    // This creates the GNOME-like "zoom from thumbnail" effect
    if (idx >= 0 && idx < (int)ws_geos.size())
      desktop_anim.warp(ws_geos[idx]);
    auto og = output->get_layout_geometry();
    desktop_anim.set_duration(anim_duration);
    desktop_anim.animate_to({0, 0, og.width, og.height});
    is_animating = true;
  }

  void cleanup_all() {
    wf::gles::run_in_context_if_gles([&] { for (auto &wd : ws_windows) for (auto &s : wd.slots) s.icon.destroy(); });
    for (int i = 0; i < (int)ws_windows.size(); i++) detach_transformers_for_ws(i);
    ws_windows.clear();
  }

  void navigate_to(int ws_idx) {
    if (ws_idx < 0 || ws_idx >= total_ws || ws_idx == focused_ws) return;
    focused_ws = ws_idx;
    carousel_scroll.set_duration(anim_duration);
    carousel_scroll.animate_to(scroll_target_for(ws_idx));
    is_animating = true;
  }

  float scroll_target_for(int ws_idx) const {
    auto og = output->get_layout_geometry();
    float ws_cx = ws_idx * (preview_geo.width + carousel_gap) + preview_geo.width / 2.0f;
    return ws_cx - og.width / 2.0f;
  }

  void tick() {
    desktop_anim.tick(); carousel_scroll.tick();
    for (auto &wd : ws_windows) for (auto &s : wd.slots) { s.anim.tick(); s.update_transformer(); if (s.view && s.view->is_mapped()) s.view->damage(); }
    check_done();
  }

  void check_done() {
    if (!is_animating) return;
    bool any = desktop_anim.is_animating() || carousel_scroll.is_animating();
    for (auto &wd : ws_windows) { for (auto &s : wd.slots) if (s.anim.is_animating()) { any = true; break; } if (any) break; }
    if (any) return;
    is_animating = false;
    auto og = output->get_layout_geometry(); auto cur = desktop_anim.current();
    if (cur.width >= og.width - 10) {
      if (switching_ws && pending_ws >= 0) { output->wset()->set_workspace(ws_index_to_point(pending_ws)); switching_ws = false; pending_ws = -1; }
      cleanup_all(); is_active = false;
    }
  }

  static void gnome_grid(int n, float area_aspect, int &cols, int &rows) {
    if (n <= 0) { cols = rows = 1; return; } if (n == 1) { cols = rows = 1; return; }
    if (n == 2) { cols = 2; rows = 1; return; } if (n == 3) { cols = 3; rows = 1; return; }
    float best_score = 1e9f; int best_c = 2, best_r = 1;
    for (int c = 2; c <= std::min(n, 6); c++) {
      int r = (n + c - 1) / c; float ga = (float)c / r;
      float rd = std::abs(ga - area_aspect) / area_aspect;
      float ep = (float)(c * r - n) / n * 0.5f; float sc = rd + ep;
      if (sc < best_score) { best_score = sc; best_c = c; best_r = r; }
    }
    cols = best_c; rows = best_r;
  }

  void arrange() {
    auto og = output->get_layout_geometry();

    int th = og.height * 0.10;
    int tw = th * og.width / og.height;
    int ws_sp = spacing / 2;
    int total_w = total_ws * tw + (total_ws - 1) * ws_sp;
    int ws_x = (og.width - total_w) / 2;
    int ws_y = og.height - spacing * 2 - th;

    ws_geos.clear();
    for (int i = 0; i < total_ws; i++)
      ws_geos.push_back({ws_x + i * (tw + ws_sp), ws_y, tw, th});

    int top = panel_height + spacing;
    int main_bot = ws_y - spacing;
    int avail_h = main_bot - top;
    int avail_w = og.width - spacing * 4;
    float aspect = (float)og.width / og.height;
    int mw = avail_w, mh = (int)(mw / aspect);
    if (mh > avail_h) { mh = avail_h; mw = (int)(mh * aspect); }
    mw = (int)(mw * 0.98); mh = (int)(mh * 0.98);
    int mx = (og.width - mw) / 2;
    int my = top + (avail_h - mh) / 2;
    preview_geo = {mx, my, mw, mh};

    carousel_gap = spacing * 2;

    for (int wi = 0; wi < total_ws; wi++) arrange_ws_windows(wi);
  }

  void arrange_ws_windows(int wi) {
    if (wi < 0 || wi >= (int)ws_windows.size()) return;
    auto &wd = ws_windows[wi]; if (wd.slots.empty()) return;
    auto og = output->get_layout_geometry();
    int n = (int)wd.slots.size(); int cols, rows;
    gnome_grid(n, (float)preview_geo.width / preview_geo.height, cols, rows);

    int inset_x = spacing, inset_y = spacing, inset_bot = spacing;
    int waw = og.width - inset_x * 2, wah = og.height - inset_y - inset_bot, gap = spacing;
    int cw = (waw - gap * (cols - 1)) / cols, ch = (wah - gap * (rows - 1)) / rows;
    int gh = rows * ch + (rows - 1) * gap;
    int gy = inset_y + (wah - gh) / 2;

    for (int i = 0; i < n; i++) {
      auto &s = wd.slots[i]; int row = i / cols;
      int itr = (row == rows - 1) ? (n - row * cols) : cols;
      int row_w = itr * cw + (itr - 1) * gap;
      int row_x = (og.width - row_w) / 2;
      int cir = i - row * cols;
      int cx = row_x + cir * (cw + gap), cy = gy + row * (ch + gap);
      double sc = std::min((double)cw / s.orig_geo.width, (double)ch / s.orig_geo.height) * 0.95;
      int sw = (int)(s.orig_geo.width * sc), sh = (int)(s.orig_geo.height * sc);
      s.target_geo = { cx + (cw - sw) / 2, cy + (ch - sh) / 2, sw, sh };
    }
  }

  // Get large preview render geo for workspace ws_idx
  // Returns in output-local screen coordinates (Y-down)
  // scroll_target_for() already centers the focused ws, so no extra offset needed
  wf::geometry_t get_large_ws_render_geo(int ws_idx) const {
    float scroll = carousel_scroll.value();
    float base_x = ws_idx * (preview_geo.width + carousel_gap);
    float rx = base_x - scroll;
    return {(int)rx, preview_geo.y, preview_geo.width, preview_geo.height};
  }

  // Hit testing: screen-local point — NO Y-flip needed, coords match screen space
  int find_large_ws_at(wf::pointf_t screen_local) {
    for (int i = 0; i < total_ws; i++) {
      auto g = get_large_ws_render_geo(i);
      if (screen_local.x >= g.x && screen_local.x < g.x + g.width &&
          screen_local.y >= g.y && screen_local.y < g.y + g.height) return i;
    }
    return -1;
  }

  int find_thumb_ws_at(wf::pointf_t global_p) {
    auto og = output->get_layout_geometry();
    float lx = global_p.x - og.x, ly = og.height - (global_p.y - og.y);
    for (size_t i = 0; i < ws_geos.size(); i++) {
      auto &g = ws_geos[i];
      if (lx >= g.x && lx < g.x + g.width && ly >= g.y && ly < g.y + g.height) return i;
    }
    return -1;
  }

  // Map screen-local → workspace-local (same math as original screen_to_workspace)
  wf::pointf_t screen_to_ws_local(wf::pointf_t screen_local, int ws_idx) {
    auto og = output->get_layout_geometry();
    auto dg = get_large_ws_render_geo(ws_idx);
    if (dg.width <= 0 || dg.height <= 0) return screen_local;
    return {
      (screen_local.x - dg.x) * (float)og.width / dg.width,
      (screen_local.y - (float)og.height + dg.y + dg.height) * (float)og.height / dg.height
    };
  }

  int find_slot_at(wf::pointf_t screen_local) {
    if (focused_ws < 0 || focused_ws >= (int)ws_windows.size()) return -1;
    if (find_large_ws_at(screen_local) != focused_ws) return -1;
    auto wp = screen_to_ws_local(screen_local, focused_ws);
    auto &slots = ws_windows[focused_ws].slots;
    for (int i = (int)slots.size() - 1; i >= 0; i--) {
      auto g = slots[i].anim.current();
      if (wp.x >= g.x && wp.x < g.x + g.width && wp.y >= g.y && wp.y < g.y + g.height) return i;
    }
    return -1;
  }

  wayfire_toplevel_view find_view_at(wf::pointf_t screen_local) {
    int idx = find_slot_at(screen_local);
    if (idx >= 0 && focused_ws >= 0 && focused_ws < (int)ws_windows.size())
      return ws_windows[focused_ws].slots[idx].view;
    return nullptr;
  }

  // Drag
  bool start_drag(wf::pointf_t sl) {
    if (drag.active) return false;
    int idx = find_slot_at(sl);
    if (idx < 0 || focused_ws < 0 || focused_ws >= (int)ws_windows.size()) return false;
    auto &s = ws_windows[focused_ws].slots[idx]; auto cur = s.anim.current();
    drag.active = true; drag.view = s.view; drag.slot_index = idx;
    drag.grab_cursor = sl; drag.current_cursor = sl; drag.hover_ws = -1; drag.hover_large_ws = -1;
    auto dg = get_large_ws_render_geo(focused_ws); auto og = output->get_layout_geometry();
    float sx = (float)dg.width / og.width, sy = (float)dg.height / og.height;
    drag.initial_screen_geo = { (int)(dg.x + cur.x * sx), (int)((float)og.height - dg.y - dg.height + cur.y * sy), (int)(cur.width * sx), (int)(cur.height * sy) };
    drag.float_width = drag.initial_screen_geo.width; drag.float_height = drag.initial_screen_geo.height;
    drag.view_geo = s.view->get_geometry();
    drag.grab_offset_in_window = { sl.x - (float)drag.initial_screen_geo.x, sl.y - (float)drag.initial_screen_geo.y };
    drag.needs_capture = true; drag.has_snapshot = false;
    return true;
  }

  void update_drag(wf::pointf_t sl, wf::pointf_t gp) {
    if (!drag.active) return;
    drag.current_cursor = sl; drag.hover_ws = find_thumb_ws_at(gp);
    drag.hover_large_ws = find_large_ws_at(sl);
    if (drag.hover_large_ws == focused_ws) drag.hover_large_ws = -1;
  }

  bool end_drag(wf::pointf_t sl, wf::pointf_t gp) {
    if (!drag.active) return false;
    int tw = find_thumb_ws_at(gp), tl = find_large_ws_at(sl);
    if (tl == focused_ws) tl = -1;
    int at = -1;
    if (tw >= 0 && tw != focused_ws) at = tw; else if (tl >= 0) at = tl;
    if (at >= 0 && drag.view) {
      auto moved_view = drag.view;
      move_view_to_workspace(moved_view, at);
      // Remove from source workspace
      if (drag.slot_index >= 0 && drag.slot_index < (int)ws_windows[focused_ws].slots.size()) {
        auto &s = ws_windows[focused_ws].slots[drag.slot_index];
        if (s.view && s.view->is_mapped() && s.transformer) { s.reset_transformer(); s.view->get_transformed_node()->rem_transformer(TRANSFORMER_NAME); }
        wf::gles::run_in_context([&] { s.icon.destroy(); });
        ws_windows[focused_ws].slots.erase(ws_windows[focused_ws].slots.begin() + drag.slot_index);
      }
      drag.reset();
      // Rearrange source workspace
      rearrange_focused_ws();
      // Add to destination workspace and rearrange it
      add_view_to_ws(moved_view, at);
      return true;
    }
    if (drag.slot_index >= 0 && focused_ws >= 0 && focused_ws < (int)ws_windows.size() && drag.slot_index < (int)ws_windows[focused_ws].slots.size()) {
      auto &s = ws_windows[focused_ws].slots[drag.slot_index];
      if (s.transformer) s.transformer->alpha = 0.88f;
      s.anim.set_duration(anim_duration); s.anim.animate_to(s.target_geo); is_animating = true;
    }
    drag.reset(); return false;
  }

  void cancel_drag() {
    if (!drag.active) return;
    if (drag.slot_index >= 0 && focused_ws >= 0 && focused_ws < (int)ws_windows.size() && drag.slot_index < (int)ws_windows[focused_ws].slots.size()) {
      auto &s = ws_windows[focused_ws].slots[drag.slot_index];
      if (s.transformer) s.transformer->alpha = 0.88f;
      s.anim.set_duration(anim_duration); s.anim.animate_to(s.target_geo); is_animating = true;
    }
    drag.reset();
  }

  void move_view_to_workspace(wayfire_toplevel_view view, int wi) {
    if (!view || !view->is_mapped()) return;
    auto t = ws_index_to_point(wi); auto c = output->wset()->get_current_workspace();
    auto og = output->get_layout_geometry(); auto vg = view->get_geometry();
    view->move(vg.x + (t.x - c.x) * og.width, vg.y + (t.y - c.y) * og.height);
  }

  void rearrange_focused_ws() {
    if (focused_ws < 0 || focused_ws >= (int)ws_windows.size()) return;
    arrange_ws_windows(focused_ws);
    for (auto &s : ws_windows[focused_ws].slots) { s.anim.set_duration(anim_duration); s.anim.animate_to(s.target_geo); }
    is_animating = true;
  }

  void rearrange_ws(int wi) {
    if (wi < 0 || wi >= (int)ws_windows.size()) return;
    arrange_ws_windows(wi);
    for (auto &s : ws_windows[wi].slots) { s.anim.set_duration(anim_duration); s.anim.animate_to(s.target_geo); }
    is_animating = true;
  }

  void add_view_to_ws(wayfire_toplevel_view view, int dest_ws) {
    if (!view || !view->is_mapped()) return;
    if (dest_ws < 0 || dest_ws >= (int)ws_windows.size()) return;
    auto og = output->get_layout_geometry();
    auto wsp = ws_index_to_point(dest_ws);
    int ox = (wsp.x - cur_ws.x) * og.width;
    int oy = (wsp.y - cur_ws.y) * og.height;
    auto vg = view->get_geometry();

    window_slot_t s;
    s.view = view;
    s.orig_geo = {vg.x - ox, vg.y - oy, vg.width, vg.height};
    if (s.orig_geo.width <= 0) s.orig_geo.width = 100;
    if (s.orig_geo.height <= 0) s.orig_geo.height = 100;
    s.app_id = view->get_app_id();
    s.app_name = window_slot_t::make_app_name(view);

    ws_windows[dest_ws].slots.push_back(s);
    int idx = (int)ws_windows[dest_ws].slots.size() - 1;

    // Attach transformer
    auto &ns = ws_windows[dest_ws].slots[idx];
    ns.transformer = std::make_shared<wf::scene::view_2d_transformer_t>(ns.view);
    ns.view->get_transformed_node()->add_transformer(ns.transformer, wf::TRANSFORMER_2D, TRANSFORMER_NAME);

    // Initialize animation at orig_geo so rearrange can animate to target
    ns.anim.set_duration(anim_duration);
    ns.anim.warp(ns.orig_geo);

    // Load icon
    wf::gles::run_in_context([&] {
      if (!ns.icon.load_for_app(ns.app_id, icon_size))
        ns.icon.create_fallback(ns.app_name, icon_size);
    });

    // Rearrange destination workspace and animate all slots to new positions
    rearrange_ws(dest_ws);
  }

  bool handle_click(wf::pointf_t gp) {
    auto og = output->get_layout_geometry();
    wf::pointf_t lp = {gp.x - og.x, gp.y - og.y};
    if (auto v = find_view_at(lp)) { deactivate(); return true; }
    int lws = find_large_ws_at(lp);
    if (lws >= 0 && lws != focused_ws) { navigate_to(lws); return true; }
    int ws = find_thumb_ws_at(gp);
    if (ws >= 0 && ws < (int)ws_geos.size()) {
      int c = cur_ws.y * ws_cols + cur_ws.x;
      if (ws != c) deactivate_to_ws(ws); else deactivate();
      return true;
    }
    deactivate(); return true;
  }

  void update_hover(wf::pointf_t sl) {
    if (drag.active) return;
    auto nv = find_view_at(sl);
    if (nv != hovered_view) {
      if (focused_ws >= 0 && focused_ws < (int)ws_windows.size())
        for (auto &s : ws_windows[focused_ws].slots) s.hovered = (s.view == nv);
      hovered_view = nv;
    }
  }

  wf::geometry_t get_preview_geo_output() const {
    auto og = output->get_layout_geometry();
    return {og.x + preview_geo.x, og.y + preview_geo.y, preview_geo.width, preview_geo.height};
  }

  int get_animating_ws() const {
    if (switching_ws && pending_ws >= 0) return pending_ws;
    return focused_ws;
  }
};

// ============================================================================
// GL Render Helpers (unchanged from original)
// ============================================================================

struct gl_programs_t {
  OpenGL::program_t tex, rounded, col; bool loaded = false;
  void load() {
    if (loaded) return; loaded = true;
    const char *tv = "#version 100\nattribute vec2 position; attribute vec2 uv; varying vec2 vuv; uniform mat4 matrix;\nvoid main() { gl_Position = matrix * vec4(position, 0.0, 1.0); vuv = uv; }\n";
    const char *tf = "#version 100\nprecision mediump float; varying vec2 vuv; uniform sampler2D smp; uniform float alpha;\nvoid main() { vec4 c = texture2D(smp, vuv); gl_FragColor = vec4(c.rgb * alpha, c.a * alpha); }\n";
    tex.compile(tv, tf);
    const char *rv = "#version 100\nprecision mediump float; attribute vec2 position; attribute vec2 uv; varying vec2 vuv; varying vec2 fc; uniform mat4 matrix; uniform vec2 size;\nvoid main() { gl_Position = matrix * vec4(position, 0.0, 1.0); vuv = uv; fc = uv * size; }\n";
    const char *rf = "#version 100\nprecision mediump float; varying vec2 vuv; varying vec2 fc; uniform sampler2D smp; uniform float alpha; uniform float radius; uniform vec2 size;\nvoid main() { vec4 c = texture2D(smp, vuv); vec2 cd; if (fc.x < radius && fc.y < radius) cd = fc - vec2(radius); else if (fc.x > size.x - radius && fc.y < radius) cd = fc - vec2(size.x - radius, radius); else if (fc.x < radius && fc.y > size.y - radius) cd = fc - vec2(radius, size.y - radius); else if (fc.x > size.x - radius && fc.y > size.y - radius) cd = fc - vec2(size.x - radius, size.y - radius); else { gl_FragColor = vec4(c.rgb * alpha, c.a * alpha); return; } float d = length(cd); float aa = smoothstep(radius, radius - 1.5, d); gl_FragColor = vec4(c.rgb * alpha * aa, c.a * alpha * aa); }\n";
    rounded.compile(rv, rf);
    const char *cv = "#version 100\nattribute vec2 position; uniform mat4 matrix;\nvoid main() { gl_Position = matrix * vec4(position, 0.0, 1.0); }\n";
    const char *cf = "#version 100\nprecision mediump float; uniform vec4 color;\nvoid main() { gl_FragColor = color; }\n";
    col.compile(cv, cf);
  }
  void free() { tex.free_resources(); rounded.free_resources(); col.free_resources(); }
};

inline void render_tex(OpenGL::program_t &prog, wf::output_t *out, GLuint tex, wf::geometry_t box, float alpha, bool flip_y) {
  auto og = out->get_layout_geometry();
  glm::mat4 ortho = glm::ortho<float>(og.x, og.x + og.width, og.y + og.height, og.y, -1, 1);
  float v0 = flip_y ? 1.0f : 0.0f, v1 = flip_y ? 0.0f : 1.0f;
  GLfloat verts[] = {(float)box.x, (float)box.y, (float)(box.x+box.width), (float)box.y, (float)(box.x+box.width), (float)(box.y+box.height), (float)box.x, (float)(box.y+box.height)};
  GLfloat uvs[] = {0, v0, 1, v0, 1, v1, 0, v1};
  prog.use(wf::TEXTURE_TYPE_RGBA); prog.uniformMatrix4f("matrix", ortho);
  prog.uniform1i("smp", 0); prog.uniform1f("alpha", alpha);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
  prog.attrib_pointer("position", 2, 0, verts); prog.attrib_pointer("uv", 2, 0, uvs);
  glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  glDisable(GL_BLEND); glBindTexture(GL_TEXTURE_2D, 0); prog.deactivate();
}

inline void render_rounded(OpenGL::program_t &prog, wf::output_t *out, GLuint tex, wf::geometry_t box, float alpha, float radius, bool flip_y) {
  auto og = out->get_layout_geometry();
  glm::mat4 ortho = glm::ortho<float>(og.x, og.x + og.width, og.y + og.height, og.y, -1, 1);
  float v0 = flip_y ? 1.0f : 0.0f, v1 = flip_y ? 0.0f : 1.0f;
  GLfloat verts[] = {(float)box.x, (float)box.y, (float)(box.x+box.width), (float)box.y, (float)(box.x+box.width), (float)(box.y+box.height), (float)box.x, (float)(box.y+box.height)};
  GLfloat uvs[] = {0, v0, 1, v0, 1, v1, 0, v1};
  prog.use(wf::TEXTURE_TYPE_RGBA); prog.uniformMatrix4f("matrix", ortho);
  prog.uniform1i("smp", 0); prog.uniform1f("alpha", alpha);
  prog.uniform1f("radius", radius); prog.uniform2f("size", box.width, box.height);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
  prog.attrib_pointer("position", 2, 0, verts); prog.attrib_pointer("uv", 2, 0, uvs);
  glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  glDisable(GL_BLEND); glBindTexture(GL_TEXTURE_2D, 0); prog.deactivate();
}

inline void render_rect(OpenGL::program_t &prog, wf::output_t *out, wf::geometry_t box, glm::vec4 color) {
  auto og = out->get_layout_geometry();
  glm::mat4 ortho = glm::ortho<float>(og.x, og.x + og.width, og.y + og.height, og.y, -1, 1);
  GLfloat verts[] = {(float)box.x, (float)box.y, (float)(box.x+box.width), (float)box.y, (float)(box.x+box.width), (float)(box.y+box.height), (float)box.x, (float)(box.y+box.height)};
  prog.use(wf::TEXTURE_TYPE_RGBA); prog.uniformMatrix4f("matrix", ortho);
  prog.uniform4f("color", color); prog.attrib_pointer("position", 2, 0, verts);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  glDisable(GL_BLEND); prog.deactivate();
}

// ============================================================================
// Panel Render Node
// ============================================================================

class panel_node_t : public wf::scene::node_t {
public:
  wf::output_t *output; top_panel_t *panel; gl_programs_t *progs; bool *overview_active;

  class instance_t : public wf::scene::render_instance_t {
    panel_node_t *self; wf::scene::damage_callback push_damage;
  public:
    instance_t(panel_node_t *s, wf::scene::damage_callback pd) : self(s), push_damage(pd) {}
    void schedule_instructions(std::vector<wf::scene::render_instruction_t> &instr,
                               const wf::render_target_t &target, wf::region_t &damage) override {
      if (self->overview_active && *self->overview_active) return;
      auto bbox = self->get_bounding_box();
      wf::region_t our_damage = damage & bbox;
      if (!our_damage.empty()) instr.push_back({.instance = this, .target = target, .damage = our_damage});
    }
    void render(const wf::scene::render_instruction_t &data) override {
      data.pass->custom_gles_subpass([&] {
        if (self->panel && self->panel->tex_id)
          render_tex(self->progs->tex, self->output, self->panel->tex_id, self->panel->get_render_geometry(), 1.0f, true);
      });
    }
    void compute_visibility(wf::output_t*, wf::region_t&) override {}
  };

  panel_node_t(wf::output_t *out, top_panel_t *p, gl_programs_t *pr, bool *active)
    : node_t(false), output(out), panel(p), progs(pr), overview_active(active) {}
  void gen_render_instances(std::vector<wf::scene::render_instance_uptr> &i,
                            wf::scene::damage_callback pd, wf::output_t *on) override {
    if (on != output) return;
    i.push_back(std::make_unique<instance_t>(this, pd));
  }
  wf::geometry_t get_bounding_box() override { return panel->get_geometry(); }
};

// ============================================================================
// Overview Render Node — renders the full overview with workspace carousel
// ============================================================================

class overview_node_t : public wf::scene::node_t {
public:
  wf::output_t *output; activities_view_t *activities; gl_programs_t *progs;
  GLuint wallpaper_tex; top_panel_t *panel;

  struct ws_capture_t {
    std::shared_ptr<wf::workspace_stream_node_t> stream;
    std::vector<wf::scene::render_instance_uptr> instances;
    wf::region_t damage; wf::auxilliary_buffer_t fb; wf::point_t ws;
  };

  class render_instance_t : public wf::scene::render_instance_t {
    std::shared_ptr<overview_node_t> self; wf::scene::damage_callback push_damage;
  public:
    std::vector<ws_capture_t> captures;

    render_instance_t(overview_node_t *s, wf::scene::damage_callback pd) : push_damage(pd) {
      self = std::dynamic_pointer_cast<overview_node_t>(s->shared_from_this());
      auto wsize = s->output->wset()->get_workspace_grid_size();
      for (int y = 0; y < wsize.height; y++) {
        for (int x = 0; x < wsize.width; x++) {
          ws_capture_t c; c.ws = {x, y};
          c.stream = std::make_shared<wf::workspace_stream_node_t>(s->output, c.ws);
          auto idx = captures.size();
          c.stream->gen_render_instances(c.instances, [this, idx](const wf::region_t &d) {
            if (idx < captures.size()) captures[idx].damage |= d;
            push_damage(self->get_bounding_box());
          }, s->output);
          c.damage |= c.stream->get_bounding_box();
          captures.push_back(std::move(c));
        }
      }
    }

    void capture_drag_snapshot(float scale) {
      auto &drg = self->activities->drag;
      if (!drg.needs_capture || drg.slot_index < 0) return;
      int fws = self->activities->focused_ws;
      if (fws < 0 || fws >= (int)self->activities->ws_windows.size()) return;
      if (drg.slot_index >= (int)self->activities->ws_windows[fws].slots.size()) return;
      auto &s = self->activities->ws_windows[fws].slots[drg.slot_index];
      if (!s.view || !s.view->is_mapped() || !s.transformer) { drg.needs_capture = false; return; }
      auto geo = drg.view_geo;
      if (geo.width <= 0 || geo.height <= 0) { drg.needs_capture = false; return; }

      drg.snapshot_fb.allocate({geo.width, geo.height}, scale);
      float sa = s.transformer->alpha, ssx = s.transformer->scale_x, ssy = s.transformer->scale_y;
      float stx = s.transformer->translation_x, sty = s.transformer->translation_y;
      s.transformer->alpha = 1.0f; s.transformer->scale_x = s.transformer->scale_y = 1.0f;
      s.transformer->translation_x = s.transformer->translation_y = 0;

      std::vector<wf::scene::render_instance_uptr> vi;
      s.view->get_transformed_node()->gen_render_instances(vi, [](const wf::region_t&){}, self->output);
      wf::render_target_t st{drg.snapshot_fb}; st.geometry = geo; st.scale = scale;
      wf::render_pass_params_t sp;
      sp.instances = &vi; sp.damage = wf::region_t{geo};
      sp.reference_output = self->output; sp.target = st;
      sp.flags = wf::RPASS_CLEAR_BACKGROUND;
      wf::render_pass_t::run(sp);

      s.transformer->alpha = 0.001f; s.transformer->scale_x = ssx; s.transformer->scale_y = ssy;
      s.transformer->translation_x = stx; s.transformer->translation_y = sty;
      drg.needs_capture = false; drg.has_snapshot = true;
    }

    void schedule_instructions(std::vector<wf::scene::render_instruction_t> &instr,
                               const wf::render_target_t &target, wf::region_t &damage) override {
      auto bbox = self->get_bounding_box();
      float scale = self->output->handle->scale;
      if (self->activities->drag.needs_capture) capture_drag_snapshot(scale);

      bool force = self->activities->is_animating || self->activities->drag.active
                   || self->activities->carousel_scroll.is_animating();
      for (auto &c : captures) {
        auto wb = c.stream->get_bounding_box();
        c.fb.allocate(wf::dimensions(wb), scale);
        wf::render_target_t t{c.fb}; t.geometry = wb; t.scale = scale;
        wf::render_pass_params_t p;
        p.instances = &c.instances; p.damage = force ? wf::region_t{wb} : c.damage;
        p.reference_output = self->output; p.target = t;
        p.flags = wf::RPASS_CLEAR_BACKGROUND | wf::RPASS_EMIT_SIGNALS;
        wf::render_pass_t::run(p); c.damage.clear();
      }
      instr.push_back({.instance = this, .target = target, .damage = damage & bbox});
      damage ^= bbox;
    }

    void render(const wf::scene::render_instruction_t &data) override { self->do_render(data, captures); }
    void compute_visibility(wf::output_t *out, wf::region_t &) override {
      for (auto &c : captures) for (auto &i : c.instances) { wf::region_t r = c.stream->get_bounding_box(); i->compute_visibility(out, r); }
    }
  };

  overview_node_t(wf::output_t *out, activities_view_t *act, gl_programs_t *p, GLuint wp, top_panel_t *pnl)
    : node_t(false), output(out), activities(act), progs(p), wallpaper_tex(wp), panel(pnl) {}

  void gen_render_instances(std::vector<wf::scene::render_instance_uptr> &i, wf::scene::damage_callback pd, wf::output_t *on) override {
    if (on != output) return;
    i.push_back(std::make_unique<render_instance_t>(this, pd));
  }
  wf::geometry_t get_bounding_box() override { return output->get_layout_geometry(); }

  void do_render(const wf::scene::render_instruction_t &data, std::vector<ws_capture_t> &caps) {
    data.pass->custom_gles_subpass([&] {
      auto og = output->get_layout_geometry();
      glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);

      float cr = activities->corner_radius;
      auto &drg = activities->drag;
      int total = activities->total_ws;

      // Wallpaper + dark overlay
      if (wallpaper_tex) {
        wf::geometry_t bg = {og.x, og.y, og.width, og.height};
        render_tex(progs->tex, output, wallpaper_tex, bg, 1.0f, true);
        render_rect(progs->col, output, bg, {0, 0, 0, 0.55f});
      }

      // ============================================================
      // Render small workspace thumbnails at bottom
      // ============================================================
      auto &wsg = activities->ws_geos;
      for (int i = 0; i < (int)wsg.size() && i < (int)caps.size(); i++) {
        wf::geometry_t g = {og.x + wsg[i].x, og.y + wsg[i].y, wsg[i].width, wsg[i].height};
        float a = (i == activities->focused_ws) ? 1.0f : 0.5f;
        if (drg.active && drg.hover_ws == i && i != activities->focused_ws) a = 0.9f;
        auto t = wf::gles_texture_t::from_aux(caps[i].fb);

        if (drg.active && drg.hover_ws == i && i != activities->focused_ws) {
          wf::geometry_t border = {g.x - 2, g.y - 2, g.width + 4, g.height + 4};
          render_rect(progs->col, output, border, {0.3f, 0.55f, 1.0f, 0.6f});
        }
        if (i == activities->focused_ws) {
          wf::geometry_t border = {g.x - 1, g.y - 1, g.width + 2, g.height + 2};
          render_rect(progs->col, output, border, {1.0f, 1.0f, 1.0f, 0.25f});
        }
        render_rounded(progs->rounded, output, t.tex_id, g, a, cr * 0.5f, true);
      }

      // ============================================================
      // Render large workspace previews
      // During zoom animation: render focused ws with desktop_anim (like paste 3)
      // When settled: render full carousel
      // ============================================================
      bool zooming = activities->desktop_anim.is_animating();
      int fws = activities->get_animating_ws();

      if (zooming) {
        // ---- ZOOM MODE: single focused workspace using desktop_anim ----
        auto dg = activities->desktop_anim.current();
        dg.x += og.x; dg.y += og.y;
        if (dg.width > 0 && dg.height > 0 && fws >= 0 && fws < (int)caps.size()) {
          float sf = (float)dg.width / og.width;
          float rad = std::clamp(cr * 2.0f * (1.0f - sf), 0.0f, cr * 2.0f);
          auto t = wf::gles_texture_t::from_aux(caps[fws].fb);
          if (rad > 1) render_rounded(progs->rounded, output, t.tex_id, dg, 1.0f, rad, true);
          else render_tex(progs->tex, output, t.tex_id, dg, 1.0f, true);

          // Icons during zoom (same as paste 3)
          if (fws < (int)activities->ws_windows.size()) {
            auto &wd = activities->ws_windows[fws];
            float sxf = (float)dg.width / og.width;
            float syf = (float)dg.height / og.height;
            int isz = activities->icon_size;
            for (auto &s : wd.slots) {
              if (!s.icon.tex_id) continue;
              if (drg.active && s.view == drg.view) continue;
              auto ws = s.anim.current();
              float ws_cx = ws.x + ws.width / 2.0f;
              float ws_bot = ws.y + ws.height;
              float icon_render_sz = isz * sxf;
              if (icon_render_sz < isz * 0.5f) icon_render_sz = isz * 0.5f;
              float irx = dg.x + ws_cx * sxf - icon_render_sz / 2.0f;
              float win_bot_ry = dg.y + dg.height * (1.0f - ws_bot / (float)og.height);
              float inset_px = 10.0f * syf;
              float iry = win_bot_ry + inset_px;
              float win_top_ry = dg.y + dg.height * (1.0f - ws.y / (float)og.height);
              float max_ry = win_top_ry - icon_render_sz - 4.0f;
              if (iry > max_ry) iry = max_ry;
              wf::geometry_t icon_box = {(int)irx, (int)iry, (int)icon_render_sz, (int)icon_render_sz};
              float icon_alpha = s.hovered ? 1.0f : 0.90f;
              render_tex(progs->tex, output, s.icon.tex_id, icon_box, icon_alpha, true);
            }
          }
        }
      } else {
        // ---- CAROUSEL MODE: all workspaces at carousel positions ----
        for (int i = 0; i < total && i < (int)caps.size(); i++) {
          auto rg = activities->get_large_ws_render_geo(i);

          // Skip if completely off-screen
          if (rg.x + rg.width < -rg.width || rg.x > og.width + rg.width) continue;

          // Convert to global screen coords
          wf::geometry_t gl_box = {og.x + rg.x, og.y + rg.y,
                                   rg.width, rg.height};

          float alpha = (i == fws) ? 1.0f : 0.65f;
          if (drg.active && drg.hover_large_ws == i) alpha = 0.95f;

          auto t = wf::gles_texture_t::from_aux(caps[i].fb);

          if (i == fws) {
            wf::geometry_t border = {gl_box.x - 2, gl_box.y - 2, gl_box.width + 4, gl_box.height + 4};
            render_rect(progs->col, output, border, {1.0f, 1.0f, 1.0f, 0.12f});
          }
          if (drg.active && drg.hover_large_ws == i) {
            wf::geometry_t border = {gl_box.x - 3, gl_box.y - 3, gl_box.width + 6, gl_box.height + 6};
            render_rect(progs->col, output, border, {0.3f, 0.55f, 1.0f, 0.5f});
          }

          render_rounded(progs->rounded, output, t.tex_id, gl_box, alpha, cr, true);

          // App icons ON windows
          if (i < (int)activities->ws_windows.size()) {
            auto &wd = activities->ws_windows[i];
            float sxf = (float)rg.width / og.width;
            float syf = (float)rg.height / og.height;
            int isz = activities->icon_size;

            for (auto &s : wd.slots) {
              if (!s.icon.tex_id) continue;
              if (drg.active && s.view == drg.view) continue;

              auto ws = s.anim.current();
              float ws_cx = ws.x + ws.width / 2.0f;
              float ws_bot = ws.y + ws.height;

              float icon_render_sz = isz * sxf;
              if (icon_render_sz < isz * 0.5f) icon_render_sz = isz * 0.5f;

              float irx = gl_box.x + ws_cx * sxf - icon_render_sz / 2.0f;
              float win_bot_ry = gl_box.y + gl_box.height * (1.0f - ws_bot / (float)og.height);
              float inset_px = 10.0f * syf;
              float iry = win_bot_ry + inset_px;
              float win_top_ry = gl_box.y + gl_box.height * (1.0f - ws.y / (float)og.height);
              float max_ry = win_top_ry - icon_render_sz - 4.0f;
              if (iry > max_ry) iry = max_ry;

              wf::geometry_t icon_box = {(int)irx, (int)iry, (int)icon_render_sz, (int)icon_render_sz};
              float icon_alpha = s.hovered ? 1.0f : 0.90f;
              if (i != fws) icon_alpha *= 0.6f;
              render_tex(progs->tex, output, s.icon.tex_id, icon_box, icon_alpha, true);
            }
          }
        }
      }

      // ============================================================
      // Panel
      // ============================================================
      if (panel && panel->tex_id)
        render_tex(progs->tex, output, panel->tex_id, panel->get_render_geometry(), 1.0f, true);

      // ============================================================
      // Floating drag thumbnail
      // ============================================================
      if (drg.active && drg.has_snapshot && drg.float_width > 0 && drg.float_height > 0) {
        auto snap_t = wf::gles_texture_t::from_aux(drg.snapshot_fb);
        float sdx = drg.current_cursor.x - drg.grab_cursor.x;
        float sdy = drg.current_cursor.y - drg.grab_cursor.y;
        float scx = drg.initial_screen_geo.x + drg.initial_screen_geo.width / 2.0f + sdx;
        float scy = drg.initial_screen_geo.y + drg.initial_screen_geo.height / 2.0f + sdy;
        int fw = drg.float_width, fh = drg.float_height;
        wf::geometry_t fb = {
          og.x + (int)(scx - fw / 2.0f),
          og.y + (int)(og.height - scy - fh / 2.0f),
          fw, fh
        };
        wf::geometry_t shadow = {fb.x + 4, fb.y - 4, fw, fh};
        render_rect(progs->col, output, shadow, {0, 0, 0, 0.35f});
        render_rounded(progs->rounded, output, snap_t.tex_id, fb, 0.95f, cr, true);
      }
    });
  }
};

// ============================================================================
// Per-Output Instance
// ============================================================================

class overview_output_t : public wf::per_output_plugin_instance_t {
public:
  std::unique_ptr<top_panel_t> panel;
  std::unique_ptr<activities_view_t> activities;
  std::shared_ptr<overview_node_t> render_node;
  std::shared_ptr<panel_node_t> panel_node;
  gl_programs_t progs;
  GLuint wallpaper_tex = 0;
  std::string wallpaper_path;
  wf::activator_callback toggle_cb;
  wf::wl_timer<false> clock_timer;
  wf::effect_hook_t pre_hook;
  bool hooks_active = false;
  int panel_height = 16, corner_radius = 12, spacing = 20, anim_duration = 300;
  std::string panel_color = "#1a1a1aE6";
  bool button_held = false;
  wf::pointf_t press_pos{0, 0};
  bool drag_started = false;
  static constexpr float DRAG_THRESHOLD = 8.0f;

  void load_wallpaper() {
    if (wallpaper_path.empty()) return;
    auto img = cairo_image_surface_create_from_png(wallpaper_path.c_str());
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(img); return; }
    int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
    auto data = cairo_image_surface_get_data(img);
    wf::gles::run_in_context([&] {
      if (!wallpaper_tex) glGenTextures(1, &wallpaper_tex);
      glBindTexture(GL_TEXTURE_2D, wallpaper_tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
      glBindTexture(GL_TEXTURE_2D, 0);
    });
    cairo_surface_destroy(img);
  }

  void init() override {
    panel = std::make_unique<top_panel_t>(output, panel_height, panel_color);
    activities = std::make_unique<activities_view_t>(output);
    activities->set_config(corner_radius, spacing, panel_height, anim_duration);
    wf::gles::run_in_context([&] { progs.load(); });
    load_wallpaper();

    pre_hook = [this]() {
      bool wa = activities->is_animating, wd = activities->drag.active;
      bool wc = activities->carousel_scroll.is_animating();
      activities->tick();
      bool sa = activities->is_animating, sd = activities->drag.active;
      bool sc = activities->carousel_scroll.is_animating();
      if (sa || sd || sc) {
        if (render_node) wf::scene::damage_node(render_node, render_node->get_bounding_box());
        output->render->schedule_redraw();
      } else if ((wa || wd || wc) && !sa && !activities->is_active) {
        output->render->damage_whole(); deactivate_hooks();
      }
    };

    clock_timer.set_timeout(60000, [this]() {
      panel->render(); panel->upload();
      if (panel_node) wf::scene::damage_node(panel_node, panel_node->get_bounding_box());
      return true;
    });

    panel_node = std::make_shared<panel_node_t>(output, panel.get(), &progs, &activities->is_active);
    wf::scene::add_front(wf::get_core().scene(), panel_node);
    wf::scene::damage_node(panel_node, panel_node->get_bounding_box());
  }

  void activate_hooks() {
    if (hooks_active) return;
    if (!render_node) {
      render_node = std::make_shared<overview_node_t>(output, activities.get(), &progs, wallpaper_tex, panel.get());
      wf::scene::add_front(wf::get_core().scene(), render_node);
    }
    if (panel_node) {
      wf::scene::remove_child(panel_node);
      wf::scene::add_front(wf::get_core().scene(), panel_node);
    }
    output->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);
    hooks_active = true; output->render->damage_whole();
  }

  void deactivate_hooks() {
    if (!hooks_active) return;
    output->render->rem_effect(&pre_hook); hooks_active = false;
    if (render_node) { wf::scene::remove_child(render_node); render_node = nullptr; }
    if (panel_node) wf::scene::damage_node(panel_node, panel_node->get_bounding_box());
    output->render->damage_whole();
    button_held = false; drag_started = false;
  }

  void toggle() {
    activities->toggle();
    if (activities->is_active) activate_hooks();
    output->render->damage_whole();
  }

  void fini() override {
    if (hooks_active) { output->render->rem_effect(&pre_hook); hooks_active = false; }
    if (render_node) { wf::scene::remove_child(render_node); render_node = nullptr; }
    if (panel_node) { wf::scene::remove_child(panel_node); panel_node = nullptr; }
    clock_timer.disconnect();
    wf::gles::run_in_context_if_gles([&] {
      progs.free();
      if (wallpaper_tex) { glDeleteTextures(1, &wallpaper_tex); wallpaper_tex = 0; }
    });
    activities.reset(); panel.reset();
  }

  void handle_motion(wf::pointf_t cursor) {
    auto og = output->get_layout_geometry();
    bool in_panel = cursor.y >= og.y && cursor.y < og.y + panel->height;
    if (in_panel) {
      if (panel->set_hover(panel->point_in_activities(cursor)))
        wf::scene::damage_node(panel_node, panel_node->get_bounding_box());
    } else if (panel->activities_hovered) {
      if (panel->set_hover(false))
        wf::scene::damage_node(panel_node, panel_node->get_bounding_box());
    }

    if (activities->is_active && !activities->is_animating) {
      wf::pointf_t local = {cursor.x - og.x, cursor.y - og.y};
      if (button_held && !drag_started) {
        float dx = cursor.x - press_pos.x, dy = cursor.y - press_pos.y;
        if (std::sqrt(dx*dx + dy*dy) > DRAG_THRESHOLD) {
          wf::pointf_t pl = {press_pos.x - og.x, press_pos.y - og.y};
          drag_started = activities->start_drag(pl);
        }
      }
      if (drag_started && activities->drag.active) {
        activities->update_drag(local, cursor);
        if (render_node) {
          wf::scene::damage_node(render_node, render_node->get_bounding_box());
          output->render->schedule_redraw();
        }
      } else {
        auto old = activities->hovered_view;
        activities->update_hover(local);
        if (old != activities->hovered_view && render_node)
          wf::scene::damage_node(render_node, render_node->get_bounding_box());
      }
    }
  }

  bool handle_button(uint32_t btn, uint32_t state, wf::pointf_t cursor) {
    if (btn != BTN_LEFT) return false;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
      if (panel->point_in_activities(cursor)) { toggle(); return true; }
      if (activities->is_active && !activities->is_animating) {
        button_held = true; press_pos = cursor; drag_started = false; return true;
      }
      return false;
    }
    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      bool wd = drag_started && activities->drag.active, wh = button_held;
      button_held = false;
      if (wd) {
        auto og = output->get_layout_geometry();
        wf::pointf_t local = {cursor.x - og.x, cursor.y - og.y};
        activities->end_drag(local, cursor); drag_started = false;
        if (render_node) { wf::scene::damage_node(render_node, render_node->get_bounding_box()); output->render->schedule_redraw(); }
        output->render->damage_whole(); return true;
      }
      drag_started = false;
      if (wh && activities->is_active && !activities->is_animating) {
        if (activities->handle_click(cursor)) { output->render->damage_whole(); return true; }
      }
      return false;
    }
    return false;
  }
};

// ============================================================================
// Main Plugin
// ============================================================================

class wayfire_overview_t : public wf::plugin_interface_t {
  wf::option_wrapper_t<int> opt_panel_height{"overview/panel_height"};
  wf::option_wrapper_t<std::string> opt_panel_color{"overview/panel_color"};
  wf::option_wrapper_t<int> opt_corner_radius{"overview/corner_radius"};
  wf::option_wrapper_t<int> opt_animation_duration{"overview/animation_duration"};
  wf::option_wrapper_t<int> opt_spacing{"overview/spacing"};
  wf::option_wrapper_t<wf::activatorbinding_t> opt_toggle{"overview/toggle"};
  wf::option_wrapper_t<std::string> opt_wallpaper{"overview/wallpaper"};

  std::map<wf::output_t*, std::unique_ptr<overview_output_t>> outputs;
  wf::signal::connection_t<wf::output_added_signal> on_output_added = [this](wf::output_added_signal *ev) { add_output(ev->output); };
  wf::signal::connection_t<wf::output_removed_signal> on_output_removed = [this](wf::output_removed_signal *ev) { remove_output(ev->output); };
  wf::signal::connection_t<wf::post_input_event_signal<wlr_pointer_motion_event>> on_motion = [this](auto*) { handle_motion(); };
  wf::signal::connection_t<wf::post_input_event_signal<wlr_pointer_button_event>> on_button = [this](auto *ev) { handle_button(ev->event); };

public:
  void init() override {
    wf::get_core().connect(&on_output_added); wf::get_core().connect(&on_output_removed);
    wf::get_core().connect(&on_motion); wf::get_core().connect(&on_button);
    for (auto &o : wf::get_core().output_layout->get_outputs()) add_output(o);
  }
  void fini() override {
    for (auto &[o, i] : outputs) { o->rem_binding(&i->toggle_cb); i->fini(); }
    outputs.clear();
  }
  void add_output(wf::output_t *out) {
    auto i = std::make_unique<overview_output_t>();
    i->panel_height = opt_panel_height; i->panel_color = (std::string)opt_panel_color;
    i->corner_radius = opt_corner_radius; i->anim_duration = opt_animation_duration;
    i->spacing = opt_spacing; i->output = out;
    i->wallpaper_path = (std::string)opt_wallpaper;
    i->init();
    auto *p = i.get();
    i->toggle_cb = [p](auto) { p->toggle(); return true; };
    out->add_activator(opt_toggle, &i->toggle_cb);
    outputs[out] = std::move(i);
  }
  void remove_output(wf::output_t *out) {
    if (outputs.count(out)) { out->rem_binding(&outputs[out]->toggle_cb); outputs[out]->fini(); outputs.erase(out); }
  }
  void handle_motion() {
    auto c = wf::get_core().get_cursor_position();
    auto o = wf::get_core().output_layout->get_output_at(c.x, c.y);
    if (o && outputs.count(o)) outputs[o]->handle_motion(c);
  }
  void handle_button(wlr_pointer_button_event *ev) {
    auto c = wf::get_core().get_cursor_position();
    auto o = wf::get_core().output_layout->get_output_at(c.x, c.y);
    if (o && outputs.count(o)) outputs[o]->handle_button(ev->button, ev->state, c);
  }
};

} // namespace overview
} // namespace wf

DECLARE_WAYFIRE_PLUGIN(wf::overview::wayfire_overview_t);
