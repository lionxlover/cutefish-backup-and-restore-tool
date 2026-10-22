// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "app/widgets.h"

#include "ui/color_id.h"
#include "ui/color_mixer.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/view.h"
#include "ui/window.h"

using namespace cutefish::ui;

namespace backup {

// The settings app's own tokens, on its 4 px grid: the page margin, a
// card's padding, and the heights of its rows.
static constexpr double kCardPadding = 16;
static constexpr double kCardSpacing = 12;
static constexpr double kRowHeight = 44;
static constexpr double kTwoLineRowHeight = 56;

// The sidebar's own: its items, their tiles, and the air around them.
static constexpr double kSidebarMargin = 12;
static constexpr double kSidebarItemHeight = 36;
static constexpr double kSidebarIconTile = 24;
static constexpr double kSidebarItemPadding = (kSidebarItemHeight - kSidebarIconTile) / 2;

Card::Card(bool padded)
{
    if (padded) {
        setPadding({kCardPadding, 12, kCardPadding, 12});
        setSpacing(kCardSpacing);
    } else {
        setSpacing(0);
    }
    setLayoutHints({.fillWidth = true});
}

void Card::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    painter.drawRoundedRect(localRect(), {theme.bigRadius, 0, themeCorners()}, theme.secondBackground);
    // The children are painted by the window's own walk over the tree.
}

PageStack::PageStack()
{
    setLayoutHints({.fillWidth = true, .fillHeight = true});
    setClipsChildren(true);
}

void PageStack::replace(std::unique_ptr<View> page)
{
    if (m_page)
        takeChild(m_page);
    m_page = page.get();
    addChild(std::move(page));
    if (geometry().width > 0 && geometry().height > 0)
        m_page->setGeometry({0, 0, geometry().width, geometry().height});
    requestLayout();
}

void PageStack::layout()
{
    if (m_page)
        m_page->setGeometry({0, 0, geometry().width, geometry().height});
}

void PageStack::paint(Painter& painter)
{
    // Opaque, as the settings app's pages are: only the sidebar keeps the
    // window's own backdrop showing through.
    painter.fillRect(localRect(), Theme::current().background);
}

SectionLabel::SectionLabel(std::string text, bool wrap)
{
    setMargins({kCardPadding, 0, kCardPadding, 0});
    setLayoutHints({.fillWidth = true});
    m_label = addChild<Label>(std::move(text));
    m_label->setWrap(wrap);
    m_label->setColor(ColorId::DisabledText);
    m_label->setLayoutHints({.fillWidth = true});
}

Section::Section(std::string title)
{
    // The label sits tight above what it names, 8 px of it, so the group
    // reads as one thing amid the page's wider rhythm.
    setSpacing(8);
    if (!title.empty())
        addChild<SectionLabel>(std::move(title));
}

// The rows of a card share their neighbours: the separator between two rows
// and the corners a press lights up depend on the rows around them.
detail::RowNeighbours detail::rowNeighbours(const View* self)
{
    detail::RowNeighbours around;
    const View* parent = self->parent();
    if (!parent)
        return around;
    bool seen = false;
    for (const auto& child : parent->children()) {
        auto* row = dynamic_cast<AbstractRow*>(child.get());
        if (!row || !child->isVisible())
            continue;
        if (child.get() == self) {
            seen = true;
            continue;
        }
        if (!seen) {
            around.first = false;
        } else {
            around.last = false;
            around.next = row;
            break;
        }
    }
    return around;
}

IconTextRow::IconTextRow(std::string icon, double size)
    : m_icon(std::move(icon))
    , m_iconSize(size)
{
    m_row = addChild<RowLayout>();
    m_row->setSpacing(12);
    m_row->setPadding({kCardPadding, 0, kCardPadding, 0});

    if (!m_icon.empty()) {
        View* iconHolder = m_row->addChild<ThemeIcon>(m_icon, m_iconSize);
        iconHolder->setLayoutHints({.verticalAlignment = Alignment::Center});
    }
    ColumnLayout* text = m_row->addChild<ColumnLayout>();
    text->setSpacing(2);
    text->setLayoutHints({.fillWidth = true});
    m_titleLabel = text->addChild<Label>();
    m_subtitleLabel = text->addChild<Label>();
    m_titleLabel->setElide(Label::Elide::Right);
    m_subtitleLabel->setElide(Label::Elide::Right);
    m_subtitleLabel->setFontSize(Theme::current().smallFontSize());
    m_subtitleLabel->setColor(ColorId::DisabledText);

    LayoutHints hints{.fillWidth = true};
    hints.preferredHeight = kRowHeight;
    setLayoutHints(hints);
    setTracksHover(true);
}

void IconTextRow::setTitle(const std::string& title)
{
    m_title = title;
    updateLabels();
}

void IconTextRow::setSubtitle(const std::string& subtitle)
{
    m_subtitle = subtitle;
    updateLabels();
}

void IconTextRow::updateLabels()
{
    m_titleLabel->setText(m_title);
    m_subtitleLabel->setText(m_subtitle);
    m_subtitleLabel->setVisible(!m_subtitle.empty());
    // A row with a second line is the settings app's taller row.
    LayoutHints hints = layoutHints();
    const double height = m_subtitle.empty() ? kRowHeight : kTwoLineRowHeight;
    if (hints.preferredHeight != height) {
        hints.preferredHeight = height;
        setLayoutHints(hints);
    }
    requestLayout();
}

void IconTextRow::setCheckable(bool checkable)
{
    if (m_checkable == checkable)
        return;
    m_checkable = checkable;
    if (checkable) {
        m_check = m_row->addChild<Icon>("check", 16);
        m_check->setLayoutHints({.verticalAlignment = Alignment::Center});
    } else {
        takeChild(m_check);
        m_check = nullptr;
    }
    updateLabels();
    setChecked(false);
}

void IconTextRow::setChecked(bool checked)
{
    m_checked = checked;
    if (m_check)
        m_check->setVisible(m_checked);
}

SizeF IconTextRow::sizeHint() const
{
    return {0, m_subtitle.empty() ? kRowHeight : kTwoLineRowHeight};
}

detail::RowNeighbours IconTextRow::neighbours() const
{
    return detail::rowNeighbours(this);
}

void IconTextRow::pointerEnter()
{
    m_hovered = true;
}

void IconTextRow::pointerLeave()
{
    m_hovered = false;
    m_pressed = false;
    requestPaint();
}

void IconTextRow::pointerPress(const PointerEvent&)
{
    m_pressed = true;
    requestPaint();
    // The separator above belongs to the row before, and a highlight hides
    // the separators it touches, as the settings app's do.
    if (detail::RowNeighbours around = neighbours(); around.next && around.next->rowPressed())
        requestPaint();
}

void IconTextRow::pointerRelease(const PointerEvent& event)
{
    const bool wasPressed = m_pressed;
    m_pressed = false;
    requestPaint();
    if (wasPressed && localRect().contains(event.position) && onClicked)
        onClicked();
}

void IconTextRow::paint(Painter& painter)
{
    // The children are painted by the window's own walk over the tree.
    const Theme& theme = Theme::current();
    const detail::RowNeighbours around = neighbours();
    const double width = geometry().width;
    const double height = geometry().height;

    // One shape only, as the settings app's rows: overlapping translucent
    // pieces would darken where they meet, so the corners that meet a
    // neighbour are pushed outside the row.
    if (m_pressed) {
        const double radius = theme.bigRadius;
        const double top = around.first ? 0 : -radius;
        const double bottom = around.last ? height : height + radius;
        painter.save();
        painter.clip({0, 0, width, height});
        painter.drawRoundedRect({0, top, width, bottom - top}, {radius, 0, themeCorners()},
                                withAlpha(ColorId::Text, theme.darkMode ? 0.05 : 0.15));
        painter.restore();
    }

    // A highlight hides the separators it touches, as the translucent fill
    // would otherwise show them through.
    if (m_separatorVisible && !around.last && !m_pressed && !(around.next && around.next->rowPressed())) {
        const double pixel = 1 / painter.scale();
        painter.drawRoundedRect({kCardPadding, height - pixel, width - 2 * kCardPadding, pixel},
                                {}, color(ColorId::Separator));
    }
}

FormRow::FormRow(std::string text, double minHeight)
    : m_minHeight(minHeight)
{
    m_row = addChild<RowLayout>();
    m_row->setSpacing(12);
    m_row->setPadding({kCardPadding, 0, kCardPadding, 0});
    m_label = m_row->addChild<Label>(std::move(text));
    m_label->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Center});

    LayoutHints hints{.fillWidth = true};
    hints.preferredHeight = m_minHeight;
    setLayoutHints(hints);
}

void FormRow::layout()
{
    m_row->setGeometry(localRect());
}

detail::RowNeighbours FormRow::neighbours() const
{
    return detail::rowNeighbours(this);
}

void FormRow::paint(Painter& painter)
{
    const detail::RowNeighbours around = neighbours();
    if (m_separatorVisible && !around.last) {
        const double width = geometry().width;
        const double pixel = 1 / painter.scale();
        painter.drawRoundedRect({kCardPadding, geometry().height - pixel, width - 2 * kCardPadding, pixel},
                                {}, color(ColorId::Separator));
    }
}

// One sidebar entry: its coloured tile and its name, with the press and the
// current backgrounds the settings app's entries carry.
class SidebarItem : public View
{
public:
    SidebarItem(const Sidebar::Entry& entry)
        : m_entry(entry)
    {
        m_glyph = addChild<Icon>(m_entry.icon, 16);
        m_glyph->setColor(Color::rgb(0xFFFFFF));
        m_title = addChild<Label>(m_entry.title);
        m_title->setFontSize(8 * 4 / 3);
        m_title->setVerticallyCentered(true);
    }

    void setCurrent(bool current)
    {
        if (current == m_current)
            return;
        m_current = current;
        requestPaint();
    }

    bool acceptsPointer() const override { return true; }

    std::function<void()> onClicked;

protected:
    void layout() override
    {
        const Theme& theme = Theme::current();
        const double height = geometry().height;
        m_tile = {kSidebarItemPadding, (height - kSidebarIconTile) / 2, kSidebarIconTile, kSidebarIconTile};
        m_glyph->setGeometry({m_tile.x + (m_tile.width - 16) / 2, m_tile.y + (m_tile.height - 16) / 2, 16, 16});
        const double x = kSidebarItemPadding + kSidebarIconTile + 8;
        m_title->setGeometry({x, 0, std::max(0.0, geometry().width - x), height});
        const Window* window = this->window();
        const bool active = !window || window->isActive();
        m_title->setColor(active ? theme.text : theme.disabledText);
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        // The tile: the entry's own colour, lit from above, white glyph on it.
        painter.fillRoundedRect(m_tile, 6, LinearGradient::vertical(m_tile, m_entry.tint.lighter(1.15), m_entry.tint));
        // The current entry carries the list's highlight under its own
        // fill, as the Qt version's list did.
        if (m_current)
            painter.drawRoundedRect(localRect(), {theme.mediumRadius, 0, themeCorners()},
                                     theme.text.withAlpha(0.05));
        if (m_pressed || m_current)
            painter.drawSmoothRectangle(localRect(), theme.mediumRadius,
                                        m_pressed ? color(ColorId::ItemPressed)
                                                  : withAlpha(ColorId::Text, theme.darkMode ? 0.1 : 0.05));
    }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button != MouseButton::Left)
            return;
        m_pressed = true;
        requestPaint();
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (!m_pressed)
            return;
        m_pressed = false;
        requestPaint();
        if (localRect().contains(event.position) && onClicked)
            onClicked();
    }

private:
    Sidebar::Entry m_entry;
    Icon* m_glyph = nullptr;
    Label* m_title = nullptr;
    RectF m_tile;
    bool m_current = false;
    bool m_pressed = false;
};

Sidebar::Sidebar()
{
    m_title = addChild<Label>(std::string());
    m_title->setFontSize(12 * 4 / 3);
    m_title->setVerticallyCentered(true);
    setLayoutHints({.fillHeight = true});
}

void Sidebar::setTitle(std::string title)
{
    m_title->setText(std::move(title));
}

void Sidebar::setEntries(std::vector<Entry> entries)
{
    m_entries = std::move(entries);
    for (View* item : m_items)
        takeChild(item);
    m_items.clear();
    for (const Entry& entry : m_entries) {
        SidebarItem* item = addChild<SidebarItem>(entry);
        item->onClicked = [this, item] {
            if (onActivated) {
                for (size_t i = 0; i < m_items.size(); ++i) {
                    if (m_items[i] == item) {
                        onActivated(int(i));
                        break;
                    }
                }
            }
        };
        m_items.push_back(item);
    }
    setCurrentIndex(m_current);
    requestLayout();
}

void Sidebar::setCurrentIndex(int index)
{
    m_current = index;
    for (size_t i = 0; i < m_items.size(); ++i)
        static_cast<SidebarItem*>(m_items[i])->setCurrent(int(i) == index);
}

void Sidebar::layout()
{
    const double width = geometry().width;
    // The name of the tool, over its pages.
    const double titleLeft = kSidebarMargin + kSidebarItemPadding;
    m_title->setColor(Theme::current().text);
    m_title->setGeometry({titleLeft, 8, std::max(0.0, width - 2 * titleLeft), 32});
    double y = 48;
    const double itemWidth = width - 2 * kSidebarMargin;
    for (View* item : m_items) {
        item->setGeometry({kSidebarMargin, y, itemWidth, kSidebarItemHeight});
        y += kSidebarItemHeight + 2;
    }
}

void Sidebar::paint(Painter& painter)
{
    // The settings app's own sidebar plate: the background, darkened a step
    // and half see-through, over the window's frosted backdrop.
    const Theme& theme = Theme::current();
    painter.fillRect(localRect(),
                     withAlpha(theme.darkMode ? lighter(ColorId::Background, 1.5)
                                              : darker(ColorId::Background, 1.05),
                               0.7));
}

} // namespace backup
