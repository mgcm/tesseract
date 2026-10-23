#include "SpacePickerView.h"

#include "icons.h"
#include "media_utils.h"
#include "roomlist_unread.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/keyboard_target.h"
#include "tk/layout.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <cmath>

namespace tesseract::views
{

namespace
{
constexpr float kHomeIconPx = 20.0f;
constexpr float kIndicatorW = 3.0f;
constexpr float kDotSize = 10.0f;
constexpr float kBadgeMinW = 16.0f;
constexpr float kBadgeH = 16.0f;
constexpr float kBadgePadX = 4.0f;
constexpr float kTabRadius = 10.0f;

bool in_rect(tk::Point p, tk::Rect r)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}
} // namespace

SpacePickerView::SpacePickerView()
{
    auto menu = std::make_unique<PopupMenu>();
    menu_ = add_child(std::move(menu));
    menu_->on_dismissed = [this] { menu_->close(); };
    menu_->on_layout_changed = [this]
    {
        if (auto* h = host())
        {
            h->request_relayout();
            h->request_repaint();
        }
    };
    rebuild_targets_();
}

void SpacePickerView::set_entries(std::vector<SpacePickerEntry> entries)
{
    if (entries == entries_)
        return;
    const bool count_changed = entries.size() != entries_.size();
    entries_ = std::move(entries);
    if (count_changed)
        rebuild_targets_();
    hover_ = pressed_ = -1;
    drag_tracker_.cancel();
    if (auto* h = host())
    {
        if (count_changed)
            h->mark_needs_relayout();
        h->request_repaint();
    }
}

void SpacePickerView::set_active_space(const std::string& space_id)
{
    if (active_ == space_id)
        return;
    active_ = space_id;
    if (auto* h = host())
        h->request_repaint();
}

void SpacePickerView::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

std::string SpacePickerView::tab_name_(int tab) const
{
    if (tab == 0)
        return tk::tr("Home (all rooms)");
    return entries_[static_cast<std::size_t>(tab - 1)].name;
}

void SpacePickerView::rebuild_targets_()
{
    // Keyboard stand-ins, one per painted tab (Home + entries).
    for (auto* t : targets_)
        remove_child(t);
    targets_.clear();
    for (int i = 0; i < tab_count(); ++i)
    {
        auto* t = add_child(tk::create_widget<tk::KeyboardTarget>(this));
        t->set_focus_ring_radius(kTabRadius + 2.0f);
        t->on_activate = [this, i] { select_tab_(i); };
        targets_.push_back(t);
    }
}

void SpacePickerView::select_tab_(int tab)
{
    if (tab == 0)
    {
        if (on_home)
            on_home();
        return;
    }
    if (tab < 0 || tab >= tab_count())
        return;
    if (on_select)
        on_select(entries_[static_cast<std::size_t>(tab - 1)].space_id);
}

tk::Size SpacePickerView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {kWidth, constraints.h};
}

tk::Rect SpacePickerView::tab_rect_(int tab) const
{
    return {bounds_.x, bounds_.y + kPadTop + kTabH * static_cast<float>(tab),
            bounds_.w, kTabH};
}

int SpacePickerView::tab_at_(tk::Point local) const
{
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
    for (int i = 0; i < tab_count(); ++i)
        if (in_rect(world, tab_rect_(i)))
            return i;
    return -1;
}

void SpacePickerView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    for (std::size_t i = 0; i < targets_.size(); ++i)
    {
        const int tab = static_cast<int>(i);
        tk::Rect r = tab_rect_(tab);
        r.x += 6.0f;
        r.w -= 12.0f;
        r.y += 2.0f;
        r.h -= 4.0f;
        targets_[i]->set_accessible_name(tab_name_(tab));
        targets_[i]->arrange(ctx, r);
    }
    // PopupMenu positions itself against its anchor, not these bounds.
    if (menu_)
        menu_->arrange(ctx, bounds_);
}

void SpacePickerView::paint_before_children(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    ctx.canvas.fill_rect(bounds_, pal.chrome_bg);
    ctx.canvas.draw_line({bounds_.x + bounds_.w - 0.5f, bounds_.y},
                         {bounds_.x + bounds_.w - 0.5f, bounds_.y + bounds_.h},
                         pal.separator, 1.0f);

    factory_ = &ctx.factory;
    theme_ = &ctx.theme;
    scale_factor_ = ctx.canvas.scale_factor();

    for (int i = 0; i < tab_count(); ++i)
        paint_tab_(ctx, i, tab_rect_(i));

    // Insertion marker while one of our tabs is dragged over the column.
    if (drop_slot_ >= 1 && drag_from_ >= 1 && drop_slot_ != drag_from_)
    {
        const tk::Rect slot = tab_rect_(drop_slot_);
        const float y = drop_slot_ < drag_from_ ? slot.y : slot.y + slot.h;
        ctx.canvas.fill_rounded_rect({slot.x + 8.0f, y - 1.5f, slot.w - 16.0f, 3.0f},
                                     1.5f, pal.accent);
    }
}

void SpacePickerView::paint_tab_(tk::PaintCtx& ctx, int i, tk::Rect row)
{
    const auto& pal = ctx.theme.palette;
    {
        const bool is_home = i == 0;
        const SpacePickerEntry* e =
            is_home ? nullptr : &entries_[static_cast<std::size_t>(i - 1)];
        const bool selected = is_home ? active_.empty() : e->space_id == active_;
        const tk::Rect cell{row.x + 6.0f, row.y + 2.0f, row.w - 12.0f, row.h - 4.0f};

        if (selected)
        {
            ctx.canvas.fill_rounded_rect(cell, kTabRadius, pal.sidebar_selected);
            ctx.canvas.fill_rounded_rect(
                {bounds_.x, row.y + 10.0f, kIndicatorW, row.h - 20.0f},
                kIndicatorW * 0.5f, pal.accent);
        }
        else if (hover_ == i)
        {
            ctx.canvas.fill_rounded_rect(cell, kTabRadius, pal.sidebar_hover);
        }

        const tk::Point centre{row.x + row.w * 0.5f, row.y + row.h * 0.5f};
        if (is_home)
        {
            const tk::Rect box{centre.x - kHomeIconPx * 0.5f,
                               centre.y - kHomeIconPx * 0.5f, kHomeIconPx, kHomeIconPx};
            home_icon_.draw(ctx.canvas, ctx.factory, kHomeSvg, box, kHomeIconPx,
                            selected ? pal.text_primary : pal.text_secondary);
            return;
        }

        const tk::Image* img = nullptr;
        if (!e->avatar_url.empty() && image_provider_)
        {
            img = image_provider_(e->avatar_url);
            if (!img && on_avatar_needed)
                on_avatar_needed(e->space_id);
        }
        draw_avatar(ctx.canvas, img, centre, kAvatar, e->name,
                    pal.avatar_initials_bg, pal.avatar_initials_text);

        const UnreadStyle style = unread_style_for(
            e->notification_count, e->highlight_count, e->unread_count, e->muted);
        if (style == UnreadStyle::None)
            return;
        const float bx = row.x + row.w - 8.0f;
        const float by = row.y + 6.0f;
        if (style == UnreadStyle::Dot)
        {
            ctx.canvas.fill_rounded_rect(
                {bx - kDotSize, by, kDotSize, kDotSize}, kDotSize * 0.5f, pal.unread_bg);
            return;
        }
        const bool mention = style == UnreadStyle::Mention;
        const std::uint64_t n = mention ? e->highlight_count : e->notification_count;
        tk::TextStyle ts{};
        ts.role = tk::FontRole::UnreadBadge;
        auto layout = ctx.factory.build_text(n > 99 ? "99+" : std::to_string(n), ts);
        const tk::Size tsz = layout->measure();
        const float pw = std::max(kBadgeMinW, tsz.w + kBadgePadX * 2.0f);
        const tk::Rect pill{bx - pw, by, pw, kBadgeH};
        ctx.canvas.fill_rounded_rect(pill, kBadgeH * 0.5f,
                                     mention ? pal.accent : pal.unread_bg);
        ctx.canvas.draw_text(*layout,
                             {pill.x + (pill.w - tsz.w) * 0.5f,
                              pill.y + (pill.h - tsz.h) * 0.5f},
                             mention ? pal.text_on_accent : pal.unread_text);
    }
}

bool SpacePickerView::on_pointer_down(tk::Point local)
{
    pressed_ = tab_at_(local);
    drag_tracker_.cancel();
    if (pressed_ >= 1) // Home isn't movable
        drag_tracker_.begin(local);
    return pressed_ >= 0;
}

void SpacePickerView::on_pointer_drag(tk::Point local)
{
    if (pressed_ < 1 || !drag_tracker_.crossed_threshold(local))
        return;
    const SpacePickerEntry& e = entries_[static_cast<std::size_t>(pressed_ - 1)];
    auto* h = host();
    if (!h)
        return;
    hover_ = -1;
    h->hide_tooltip(this);
    h->begin_drag(tk::DragPayload("pinned_space_id", e.space_id),
                  build_drag_visual_(e), local);
    // The drag now owns the gesture: no pointer-up reaches us, so this press
    // must not turn into a click.
    pressed_ = -1;
}

void SpacePickerView::on_pointer_up(tk::Point local, bool inside_self)
{
    drag_tracker_.cancel();
    const int was = pressed_;
    pressed_ = -1;
    if (was < 0 || !inside_self || tab_at_(local) != was)
        return;
    select_tab_(was);
}

int SpacePickerView::slot_at_world_y_(float world_y) const
{
    const int slot = static_cast<int>(
        std::floor((world_y - bounds_.y - kPadTop) / kTabH));
    return std::clamp(slot, 1, std::max(1, tab_count() - 1));
}

int SpacePickerView::tab_of_(const std::string& space_id) const
{
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].space_id == space_id)
            return static_cast<int>(i) + 1;
    return -1;
}

bool SpacePickerView::on_drag_enter(tk::Point, const tk::DragPayload& payload)
{
    const auto* id = payload.get_if<std::string>("pinned_space_id");
    if (!id || tab_of_(*id) < 1)
        return false;
    drag_from_ = tab_of_(*id);
    return true;
}

void SpacePickerView::on_drag_over(tk::Point local, const tk::DragPayload& payload)
{
    if (const auto* id = payload.get_if<std::string>("pinned_space_id"))
        drag_from_ = tab_of_(*id);
    const int slot = slot_at_world_y_(bounds_.y + local.y);
    if (slot != drop_slot_)
    {
        drop_slot_ = slot;
        if (auto* h = host())
            h->request_repaint();
    }
}

void SpacePickerView::on_drag_leave_target()
{
    drop_slot_ = drag_from_ = -1;
}

bool SpacePickerView::on_drop(tk::Point local, tk::DragPayload payload)
{
    drop_slot_ = drag_from_ = -1;
    const auto* id = payload.get_if<std::string>("pinned_space_id");
    if (!id)
        return false;
    const int from = tab_of_(*id);
    const int to = slot_at_world_y_(bounds_.y + local.y);
    if (from < 1)
        return false;
    if (from == to)
        return true; // dropped where it was: accepted, nothing to do

    // Move the entry, then report what it now sits in front of.
    auto moved = std::move(entries_[static_cast<std::size_t>(from - 1)]);
    entries_.erase(entries_.begin() + (from - 1));
    entries_.insert(entries_.begin() + (to - 1), std::move(moved));
    const auto idx = static_cast<std::size_t>(to - 1);
    const std::string moved_id = entries_[idx].space_id;
    const std::string before =
        idx + 1 < entries_.size() ? entries_[idx + 1].space_id : std::string{};
    if (on_reorder)
        on_reorder(moved_id, before);
    return true;
}

tk::DragVisual SpacePickerView::build_drag_visual_(const SpacePickerEntry& entry)
{
    tk::DragVisual visual;
    if (!factory_ || !theme_)
        return visual;
    constexpr float kSize = kAvatar; // exactly the size of a tab's avatar
    auto surface = factory_->create_offscreen({kSize, kSize}, scale_factor_);
    if (!surface)
        return visual;
    tk::Canvas& canvas = surface->canvas();
    canvas.clear(tk::Color::rgba(0, 0, 0, 0));
    const auto& pal = theme_->palette;
    const tk::Image* img = (!entry.avatar_url.empty() && image_provider_)
                               ? image_provider_(entry.avatar_url)
                               : nullptr;
    draw_avatar(canvas, img, {kSize * 0.5f, kSize * 0.5f}, kAvatar, entry.name,
                pal.avatar_initials_bg, pal.avatar_initials_text);
    visual.image = std::shared_ptr<tk::Image>(surface->finish());
    visual.hotspot = {kSize * 0.5f, kSize * 0.5f};
    visual.opacity = 0.9f;
    return visual;
}

bool SpacePickerView::on_pointer_move(tk::Point local)
{
    const int now = tab_at_(local);
    if (now == hover_)
        return false;
    hover_ = now;
    if (auto* h = host())
    {
        if (now >= 0)
            h->show_tooltip(this, tab_name_(now), tab_rect_(now));
        else
            h->hide_tooltip(this);
    }
    return true;
}

void SpacePickerView::on_pointer_leave()
{
    drag_tracker_.cancel();
    hover_ = pressed_ = -1;
    if (auto* h = host())
        h->hide_tooltip(this);
}

bool SpacePickerView::on_right_click(tk::Point local)
{
    const int tab = tab_at_(local);
    if (tab <= 0 || !menu_)
        return false; // Home has no menu
    const std::string space_id = entries_[static_cast<std::size_t>(tab - 1)].space_id;

    std::vector<PopupMenu::Item> items;
    PopupMenu::Item unpin;
    unpin.svg_icon    = kUnpinSvg;
    unpin.label       = tk::tr("Unpin from sidebar");
    unpin.on_selected = [this, space_id]
    {
        if (on_unpin)
            on_unpin(space_id);
    };
    items.push_back(std::move(unpin));

    const tk::Rect anchor_world{bounds_.x + local.x, bounds_.y + local.y,
                                PopupMenu::kWidth, 0.0f};
    menu_->open(std::move(items), anchor_world);
    return true;
}

void SpacePickerView::on_popup_dismiss()
{
    if (menu_)
        menu_->close();
}

} // namespace tesseract::views
