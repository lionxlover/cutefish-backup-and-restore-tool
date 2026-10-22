// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/cell_layout.h"
#include "ui/layout.h"
#include "ui/view.h"

namespace cutefish::ui {
class Label;
class Painter;
} // namespace cutefish::ui

namespace backup {

// The base of a row of a card: what paints the separator between rows and
// the press highlight needs to know its neighbours, whatever the row holds.
class AbstractRow
{
public:
    virtual ~AbstractRow() = default;
    virtual bool rowPressed() const = 0;
};

namespace detail {

// What a row knows of the rows around it in its card.
struct RowNeighbours
{
    bool first = true;
    bool last = true;
    AbstractRow* next = nullptr;
};

// The rows around one row, for the separator and the lit corners.
detail::RowNeighbours rowNeighbours(const cutefish::ui::View* self);

} // namespace detail

// A card: a column that paints the rounded panel the settings app paints
// behind its groups — the theme's second background, the big radius.
class Card : public cutefish::ui::ColumnLayout
{
public:
    explicit Card(bool padded = true);

protected:
    void paint(cutefish::ui::Painter& painter) override;
};

// One page at a time: the window swaps the wizard's steps in and out. The
// stack paints the page's opaque background, as the settings app's pages do.
class PageStack : public cutefish::ui::View
{
public:
    PageStack();

    // Takes the page out of the stack and puts another in. The last page
    // dies here, after its signals can no longer run.
    void replace(std::unique_ptr<cutefish::ui::View> page);
    cutefish::ui::View* page() const { return m_page; }

protected:
    void layout() override;
    void paint(cutefish::ui::Painter& painter) override;

private:
    cutefish::ui::View* m_page = nullptr;
};

// The settings app's section label: secondary text, inset as a card's own
// text is, so every edge on the page lines up. Wraps when asked, as the
// footnotes under a card do.
class SectionLabel : public cutefish::ui::Column
{
public:
    explicit SectionLabel(std::string text = {}, bool wrap = false);

    cutefish::ui::Label* label() const { return m_label; }

private:
    cutefish::ui::Label* m_label = nullptr;
};

// A titled group on a page: its label above its content, set tight so the
// text reads as belonging to that content and not to the next group.
class Section : public cutefish::ui::ColumnLayout
{
public:
    explicit Section(std::string title = {});
};

// A row with a theme icon beside a two-line column: the name and, below it,
// a smaller, fainter line. Rows of places and archives are built of it. The
// rows of a card sit against one another, parted by separators, and light up
// under the press the way the settings app's rows do.
class IconTextRow : public cutefish::ui::View, public AbstractRow
{
public:
    // `icon` is a freedesktop name, such as "drive-removable-media"; an
    // empty one leaves the space out. `size` is the icon's side.
    IconTextRow(std::string icon, double size = 24);

    void setTitle(const std::string& title);
    void setSubtitle(const std::string& subtitle);
    const std::string& title() const { return m_title; }

    cutefish::ui::Label* titleLabel() const { return m_titleLabel; }
    cutefish::ui::Label* subtitleLabel() const { return m_subtitleLabel; }
    void setCheckable(bool checkable);
    bool isChecked() const { return m_checked; }
    void setChecked(bool checked);

    void setSeparatorVisible(bool visible) { m_separatorVisible = visible; }
    bool rowPressed() const override { return m_pressed; }

    cutefish::ui::SizeF sizeHint() const override;

    // After the user pressed the row.
    std::function<void()> onClicked;

protected:
    bool acceptsPointer() const override { return true; }
    void pointerEnter() override;
    void pointerLeave() override;
    void pointerPress(const cutefish::ui::PointerEvent& event) override;
    void pointerRelease(const cutefish::ui::PointerEvent& event) override;
    void paint(cutefish::ui::Painter& painter) override;

private:
    detail::RowNeighbours neighbours() const;

    void updateLabels();

    cutefish::ui::RowLayout* m_row = nullptr;
    cutefish::ui::Label* m_titleLabel = nullptr;
    cutefish::ui::Label* m_subtitleLabel = nullptr;
    cutefish::ui::View* m_check = nullptr;
    std::string m_icon;
    std::string m_title;
    std::string m_subtitle;
    double m_iconSize;
    bool m_checkable = false;
    bool m_checked = false;
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_separatorVisible = true;
};

// A settings-style row: a label with a control at its end, and the row's
// separator below. The switch, combo and value rows of a card are built of
// it; the row itself is not clickable — its control is the interaction.
class FormRow : public cutefish::ui::View, public AbstractRow
{
public:
    // `minHeight` is the settings app's row height: 44 for a switch row, 52
    // for a row holding a taller control.
    explicit FormRow(std::string text, double minHeight = 44);

    cutefish::ui::Label* label() const { return m_label; }

    // Puts the control at the row's end and hands it back, centred.
    template<typename T, typename... Args>
    T* setTrailing(Args&&... args)
    {
        auto view = std::make_unique<T>(std::forward<Args>(args)...);
        T* trailing = view.get();
        cutefish::ui::LayoutHints hints = trailing->layoutHints();
        hints.verticalAlignment = cutefish::ui::Alignment::Center;
        trailing->setLayoutHints(hints);
        m_row->addChild(std::move(view));
        return trailing;
    }

    void setSeparatorVisible(bool visible) { m_separatorVisible = visible; }
    bool rowPressed() const override { return false; }

    cutefish::ui::SizeF sizeHint() const override { return {0, m_minHeight}; }

protected:
    void layout() override;
    void paint(cutefish::ui::Painter& painter) override;

private:
    detail::RowNeighbours neighbours() const;

    cutefish::ui::RowLayout* m_row = nullptr;
    cutefish::ui::Label* m_label = nullptr;
    double m_minHeight;
    bool m_separatorVisible = true;
};

// The sidebar: the settings app's own navigation, one entry a page. An
// entry's icon is a white glyph on a rounded tile of its own colour, and the
// entry the window stands on carries the list's highlight.
class Sidebar : public cutefish::ui::View
{
public:
    struct Entry
    {
        std::string title;
        std::string icon;
        cutefish::ui::Color tint;
    };

    Sidebar();

    void setTitle(std::string title);
    void setEntries(std::vector<Entry> entries);
    // Sets the entry the window stands on; no signal runs from it.
    void setCurrentIndex(int index);
    int currentIndex() const { return m_current; }

    // After the user picked an entry.
    std::function<void(int index)> onActivated;

    cutefish::ui::SizeF sizeHint() const override { return {230, 0}; }

protected:
    void layout() override;
    void paint(cutefish::ui::Painter& painter) override;

private:
    cutefish::ui::Label* m_title = nullptr;
    std::vector<Entry> m_entries;
    std::vector<cutefish::ui::View*> m_items;
    int m_current = -1;
};

} // namespace backup
