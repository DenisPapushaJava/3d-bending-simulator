#include "StrokeSegmentsDialog.h"
#include "MeasurementText.h"

#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace bending::ui {
namespace {
using namespace application;

struct Segment {
    std::size_t index;
    double from, to;
};

QColor strokeColor(std::size_t group) {
    const std::array colors{QColor("#B7DCDD"), QColor("#D9C9E8"), QColor("#E8D1A6"),
                            QColor("#BFCFE8")};
    return colors[group % colors.size()];
}

class StrokeList final : public QTreeWidget {
  public:
    explicit StrokeList(QWidget *parent) : QTreeWidget(parent) {
        setHeaderHidden(true);
        setSelectionMode(ExtendedSelection);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setDropIndicatorShown(true);
        setExpandsOnDoubleClick(false);
        setIndentation(14);
    }

    std::function<void(const StrokeList &, QTreeWidgetItem *)> onDrop;
    std::function<void(const QPoint &)> onMenu;

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override {
        const bool keyboard = event->reason() == QContextMenuEvent::Keyboard;
        auto *item = keyboard ? currentItem() : itemAt(event->pos());
        if (!item || !onMenu)
            return;
        if (!item->isSelected())
            setCurrentItem(item, 0, QItemSelectionModel::ClearAndSelect);
        onMenu(keyboard ? viewport()->mapToGlobal(visualItemRect(item).center())
                        : event->globalPos());
        event->accept();
    }

    void startDrag(Qt::DropActions) override {
        auto *dragData = new QMimeData;
        dragData->setData(mimeType, QByteArray{});
        QDrag drag(this);
        drag.setMimeData(dragData);
        drag.exec(Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (accepts(event))
            event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (!accepts(event))
            return;
        QTreeWidget::dragMoveEvent(event);
        event->acceptProposedAction();
    }
    void dropEvent(QDropEvent *event) override {
        if (!accepts(event) || !onDrop)
            return;
        event->acceptProposedAction();
        onDrop(*static_cast<const StrokeList *>(event->source()),
               itemAt(event->position().toPoint()));
    }

  private:
    static constexpr auto mimeType = "application/x-bending-stroke";
    bool accepts(const QDropEvent *event) const {
        return event->source() && event->source() != this && event->mimeData()->hasFormat(mimeType);
    }
};

class SegmentStrip final : public QWidget {
  public:
    SegmentStrip(std::span<const Segment> segments, QWidget *parent)
        : QWidget(parent), segments_(segments) {
        setMinimumHeight(72);
        setAccessibleName(QStringLiteral("Схема ходов вдоль линии гиба"));
    }

    void showStrokes(std::vector<std::size_t> groups, const BendStroke &selected) {
        groups_ = std::move(groups);
        selected_ = selected;
        update();
    }

    std::function<void(std::size_t, Qt::KeyboardModifiers)> onSelect;

  private:
    QRectF segmentRect(const Segment &segment) const {
        constexpr int margin = 12;
        const auto scale =
            (width() - 2.0 * margin) / (segments_.back().to - segments_.front().from);
        return {margin + (segment.from - segments_.front().from) * scale, 40,
                (segment.to - segment.from) * scale, 22};
    }

    void paintEvent(QPaintEvent *) override {
        if (groups_.empty())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().mid().color(), 1, Qt::DashLine));
        painter.drawLine(12, 51, width() - 12, 51);
        for (std::size_t row = 0; row < segments_.size(); ++row) {
            const auto rect = segmentRect(segments_[row]);
            const auto group = groups_[row];
            const bool selected = std::find(selected_.begin(), selected_.end(),
                                            segments_[row].index) != selected_.end();
            painter.setPen(selected ? QPen(palette().text().color(), 2) : QPen(Qt::NoPen));
            painter.setBrush(strokeColor(group));
            painter.drawRoundedRect(rect, 2, 2);
            if (row > 0 && groups_[row - 1] == group)
                continue;
            auto last = row;
            while (last + 1 < groups_.size() && groups_[last + 1] == group)
                ++last;
            const auto right = segmentRect(segments_[last]).right();
            const auto left = rect.left();
            painter.setPen(palette().text().color());
            painter.drawPolyline(QPolygonF{{left, 36}, {left, 30}, {right, 30}, {right, 36}});
            const auto label = QString::number(group + 1);
            if (right - left >= painter.fontMetrics().horizontalAdvance(label) + 2)
                painter.drawText(QRectF(left, 7, right - left, 20), Qt::AlignCenter, label);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && onSelect)
            for (const auto &segment : segments_)
                if (segmentRect(segment).contains(event->position())) {
                    onSelect(segment.index, event->modifiers());
                    return;
                }
    }

    std::span<const Segment> segments_;
    std::vector<std::size_t> groups_;
    BendStroke selected_;
};

bool sameSetup(const BendPreparation &a, const BendPreparation &b) {
    return a.tools == b.tools && a.catalogTools == b.catalogTools && a.review == b.review &&
           a.target == b.target && a.workpieceReversed == b.workpieceReversed &&
           a.workpieceOffsetMm == b.workpieceOffsetMm;
}

// A request moves complete physical units. Shared moving walls form one unit.
struct StrokeChange {
    DocumentSession draft;
    std::vector<StrokeEdit> edits;

    bool detach(const BendStroke &stroke, const BendStroke &units) {
        if (!draft.splitStroke(stroke, units))
            return false;
        edits.push_back({stroke, units, {}});
        return true;
    }

    bool merge(const BendStroke &zones, std::size_t keep) {
        if (!draft.mergeStrokes(zones, keep))
            return false;
        edits.push_back({zones, {}, keep});
        return true;
    }
};

BendStroke itemZones(const QTreeWidgetItem *item) {
    BendStroke result;
    if (!item)
        return result;
    if (item->childCount()) {
        for (int i = 0; i < item->childCount(); ++i) {
            const auto zones = itemZones(item->child(i));
            result.insert(result.end(), zones.begin(), zones.end());
        }
    } else {
        for (const auto &index : item->data(0, Qt::UserRole).toList())
            result.push_back(index.toULongLong());
    }
    return result;
}

BendStroke selectedZones(const StrokeList &list) {
    BendStroke result;
    for (const auto *item : list.selectedItems()) {
        const auto zones = itemZones(item);
        result.insert(result.end(), zones.begin(), zones.end());
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool overlaps(const BendStroke &a, const BendStroke &b) {
    return std::any_of(a.begin(), a.end(),
                       [&](auto index) { return std::find(b.begin(), b.end(), index) != b.end(); });
}
bool contains(const BendStroke &zones, std::size_t index) {
    return std::find(zones.begin(), zones.end(), index) != zones.end();
}
} // namespace

bool StrokeSegmentsDialog::edit(const DocumentSession &session, std::size_t bend,
                                const std::function<void(const BendStroke &)> &highlight) {
    const auto &analysis = session.document()->metadata().bendAnalysis;
    const auto &first = analysis.bends[bend];
    BendStroke source;
    if (first.interruptedStroke) {
        for (std::size_t i = 0; i < analysis.bends.size(); ++i)
            if (analysis.bends[i].interruptedStroke == first.interruptedStroke)
                source.push_back(i);
    } else {
        for (const auto &stroke : session.bendStrokes(session.orderedBends()))
            if (contains(stroke, bend)) {
                source = stroke;
                break;
            }
    }
    if (source.size() < 2 ||
        std::any_of(source.begin(), source.end(), [&](auto index) { return !session.bend(index); }))
        return false;

    const PointMm direction{first.axisEnd.x - first.axisStart.x,
                            first.axisEnd.y - first.axisStart.y,
                            first.axisEnd.z - first.axisStart.z};
    const auto length = std::hypot(direction.x, direction.y, direction.z);
    if (!(length > 0))
        return false;
    const auto project = [&](PointMm p) {
        return ((p.x - first.axisStart.x) * direction.x + (p.y - first.axisStart.y) * direction.y +
                (p.z - first.axisStart.z) * direction.z) /
               length;
    };
    std::vector<Segment> segments;
    for (const auto index : source) {
        const auto &zone = analysis.bends[index];
        const auto from = project(zone.axisStart), to = project(zone.axisEnd);
        segments.push_back({index, std::min(from, to), std::max(from, to)});
    }
    std::sort(segments.begin(), segments.end(),
              [](const auto &a, const auto &b) { return a.from < b.from; });
    source.clear();
    for (const auto &segment : segments)
        source.push_back(segment.index);
    const auto units = routeBendStrokes(analysis, source, false);

    selectedBend_ = bend;
    setWindowTitle(QStringLiteral("Ходы гибки"));
    resize(690, 450);
    auto *layout = new QVBoxLayout(this);
    auto *summary = new QLabel(this);
    layout->addWidget(summary);
    auto *strip = new SegmentStrip(segments, this);
    layout->addWidget(strip);
    auto *columns = new QHBoxLayout;
    auto *together = new StrokeList(this);
    auto *separate = new StrokeList(this);
    together->setObjectName(QStringLiteral("combinedStrokes"));
    separate->setObjectName(QStringLiteral("separateStrokes"));
    const auto addColumn = [&](const QString &title, StrokeList *list, const QString &allText,
                               const QString &name) {
        auto *column = new QVBoxLayout;
        auto *header = new QHBoxLayout;
        auto *label = new QLabel(title, this);
        auto font = label->font();
        font.setBold(true);
        label->setFont(font);
        header->addWidget(label, 1);
        list->setAccessibleName(title);
        auto *all = new QPushButton(allText, this);
        all->setObjectName(name);
        all->setAutoDefault(false);
        header->addWidget(all);
        column->addLayout(header);
        column->addWidget(list);
        columns->addLayout(column, 1);
        return all;
    };
    auto *mergeAll = addColumn(QStringLiteral("Гнуть вместе"), together,
                               QStringLiteral("Объединить все"), QStringLiteral("mergeAllStrokes"));
    auto *splitAll = addColumn(QStringLiteral("Гнуть отдельно"), separate,
                               QStringLiteral("Разделить все"), QStringLiteral("splitAllStrokes"));
    layout->addLayout(columns, 1);

    auto *note = new QLabel(this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *apply = buttons->button(QDialogButtonBox::Ok);
    apply->setText(QStringLiteral("Применить"));
    apply->setObjectName(QStringLiteral("applyStrokeEdits"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(buttons);
    auto *menu = new QMenu(this);
    auto *merge = menu->addAction(QStringLiteral("Гнуть вместе"));
    auto *split = menu->addAction(QStringLiteral("Гнуть отдельно"));
    merge->setObjectName(QStringLiteral("mergeSelectedStrokes"));
    split->setObjectName(QStringLiteral("splitSelectedSegments"));
    together->onMenu = separate->onMenu = [menu](const QPoint &position) { menu->popup(position); };

    StrokeChange change{session, {}};
    std::vector<BendStroke> strokes;
    std::vector<std::size_t> groupBySegment;
    const auto number = [](double value) { return measurementText(value); };
    const auto id = [](auto index) {
        return QStringLiteral("B%1").arg(index + 1, 2, 10, QLatin1Char('0'));
    };
    const auto spanText = [&](const BendStroke &zones) {
        return QStringLiteral("%1 мм").arg(number(routeStrokeLengthMm(analysis, zones)));
    };
    const auto mark = [](QTreeWidgetItem &item, std::size_t group) {
        item.setForeground(0, QBrush(QColor("#273744")));
        item.setBackground(0, QBrush(strokeColor(group)));
    };

    const auto canDetach = [&](const BendStroke &selection) {
        return std::any_of(strokes.begin(), strokes.end(), [&](const auto &stroke) {
            for (const auto &unit : units) {
                if (!overlaps(unit, selection) || !overlaps(unit, stroke) || unit == stroke)
                    continue;
                BendStroke remainder;
                for (const auto index : stroke)
                    if (!contains(unit, index))
                        remainder.push_back(index);
                if (change.draft.canSplitStroke(unit, remainder))
                    return true;
            }
            return false;
        });
    };
    const auto currentSelection = [&] {
        auto selected = selectedZones(*together);
        const auto individual = selectedZones(*separate);
        selected.insert(selected.end(), individual.begin(), individual.end());
        return selected;
    };
    const auto updateSelection = [&] {
        const auto selected = currentSelection();
        strip->showStrokes(groupBySegment, selected);
        highlight(selected.empty() ? source : selected);
        split->setEnabled(canDetach(selectedZones(*together)));
        merge->setEnabled(std::count_if(strokes.begin(), strokes.end(),
                                        [&](const auto &s) { return overlaps(s, selected); }) > 1);
    };

    const auto rebuild = [&] {
        const QSignalBlocker blockTogether(together), blockSeparate(separate);
        together->clear();
        separate->clear();
        strokes = change.draft.bendStrokes(source);
        groupBySegment.resize(source.size());
        for (std::size_t group = 0; group < strokes.size(); ++group) {
            const auto &stroke = strokes[group];
            const auto count = std::count_if(units.begin(), units.end(), [&](const auto &unit) {
                return overlaps(unit, stroke);
            });
            QTreeWidgetItem *header = nullptr;
            if (count > 1) {
                header = new QTreeWidgetItem(together);
                header->setText(0,
                                QStringLiteral("Ход %1 · %2").arg(group + 1).arg(spanText(stroke)));
                mark(*header, group);
                header->setExpanded(true);
            }
            for (const auto &unit : units) {
                if (!overlaps(stroke, unit))
                    continue;
                auto *item = header ? new QTreeWidgetItem(header) : new QTreeWidgetItem(separate);
                QVariantList values;
                QStringList labels;
                for (const auto index : unit) {
                    values.push_back(QVariant::fromValue<qulonglong>(index));
                    labels.push_back(id(index));
                    const auto at = std::find(source.begin(), source.end(), index);
                    groupBySegment[static_cast<std::size_t>(at - source.begin())] = group;
                }
                item->setData(0, Qt::UserRole, values);
                const auto text = unit.size() == 1
                                      ? labels.front()
                                      : QStringLiteral("%1 · общая полка").arg(labels.join(" + "));
                item->setText(0, header ? QStringLiteral("%1 · %2").arg(text, spanText(unit))
                                        : QStringLiteral("Ход %1 · %2 · %3")
                                              .arg(group + 1)
                                              .arg(text, spanText(unit)));
                item->setToolTip(0, item->text(0));
                if (!header)
                    mark(*item, group);
            }
        }
        together->expandAll();
        summary->setText(QStringLiteral("Ходов %1 · участков %2 · охват %3")
                             .arg(strokes.size())
                             .arg(source.size())
                             .arg(spanText(source)));
        apply->setEnabled(!change.edits.empty());
        splitAll->setEnabled(canDetach(source));
        mergeAll->setEnabled(strokes.size() > 1);
        note->setText(units.size() == 1
                          ? QStringLiteral("Участки общей полки гнутся одним ходом.")
                          : QStringLiteral("Перетащите участки или откройте меню правой кнопкой. "
                                           "Ctrl и Shift выделяют несколько строк."));
        updateSelection();
    };

    const auto commit = [&](StrokeChange candidate, std::size_t selected) {
        change.draft = std::move(candidate.draft);
        change.edits.insert(change.edits.end(), candidate.edits.begin(), candidate.edits.end());
        selectedBend_ = selected;
        rebuild();
    };
    const auto detach = [&](const BendStroke &selected) {
        StrokeChange candidate{change.draft, {}};
        for (auto stroke : strokes)
            for (const auto &unit : units) {
                if (stroke.size() <= unit.size() || !overlaps(unit, stroke) ||
                    !overlaps(unit, selected))
                    continue;
                if (candidate.detach(stroke, unit))
                    std::erase_if(stroke, [&](auto index) { return contains(unit, index); });
            }
        if (!candidate.edits.empty())
            commit(std::move(candidate), selected.front());
        else
            note->setText(QStringLiteral(
                "Выбранные участки уже идут отдельным ходом или связаны общей полкой."));
    };
    const auto combine = [&](BendStroke selected) {
        if (std::count_if(strokes.begin(), strokes.end(),
                          [&](const auto &stroke) { return overlaps(stroke, selected); }) < 2) {
            note->setText(
                QStringLiteral("Выберите участки из двух ходов или перетащите их на нужный ход."));
            return;
        }
        StrokeChange candidate{change.draft, {}};
        QStringList choices;
        BendStroke setups;
        for (std::size_t group = 0; group < strokes.size(); ++group) {
            const auto &stroke = strokes[group];
            BendStroke taken;
            for (const auto index : stroke)
                if (contains(selected, index))
                    taken.push_back(index);
            if (taken.empty())
                continue;
            if (taken.size() < stroke.size() && !candidate.detach(stroke, taken)) {
                note->setText(QStringLiteral("Перенесите участки общей полки вместе."));
                return;
            }
            const auto index = taken.front();
            const auto *entry = change.draft.bend(index);
            auto label = QStringLiteral("Ход %1 · %2").arg(group + 1).arg(id(index));
            if (entry->catalogTools) {
                const auto &tools = *entry->catalogTools;
                label += QStringLiteral(" · %1 / %2")
                             .arg(QString::fromStdString(tools.punch.code),
                                  QString::fromStdString(tools.die.code));
            }
            choices.push_back(label);
            setups.push_back(index);
        }
        auto keep = setups.front();
        if (std::any_of(selected.begin(), selected.end(), [&](auto index) {
                return !sameSetup(*change.draft.bend(keep), *change.draft.bend(index));
            })) {
            bool ok = false;
            const auto choice = QInputDialog::getItem(
                this, QStringLiteral("Настройки общего хода"),
                QStringLiteral("Настройки отличаются. Выберите, какие сохранить."), choices, 0,
                false, &ok);
            if (!ok)
                return;
            keep = setups[static_cast<std::size_t>(choices.indexOf(choice))];
        }
        if (candidate.merge(selected, keep))
            commit(std::move(candidate), keep);
    };

    QObject connections;
    connect(together, &QTreeWidget::itemSelectionChanged, &connections, updateSelection);
    connect(separate, &QTreeWidget::itemSelectionChanged, &connections, updateSelection);
    connect(splitAll, &QPushButton::clicked, &connections, [&] { detach(source); });
    connect(mergeAll, &QPushButton::clicked, &connections, [&] { combine(source); });
    connect(split, &QAction::triggered, &connections, [&] { detach(selectedZones(*together)); });
    connect(merge, &QAction::triggered, &connections, [&] { combine(currentSelection()); });
    separate->onDrop = [&](const auto &list, auto *) {
        QTimer::singleShot(0, &connections,
                           [&, selected = selectedZones(list)] { detach(selected); });
    };
    together->onDrop = [&](const auto &list, const QTreeWidgetItem *target) {
        auto selected = selectedZones(list);
        const auto zones = itemZones(target && target->parent() ? target->parent() : target);
        selected.insert(selected.end(), zones.begin(), zones.end());
        QTimer::singleShot(0, &connections, [&, selected] { combine(selected); });
    };
    strip->onSelect = [&](auto index, auto modifiers) {
        const QSignalBlocker blockTogether(together), blockSeparate(separate);
        if (!(modifiers & (Qt::ControlModifier | Qt::ShiftModifier))) {
            together->clearSelection();
            separate->clearSelection();
        }
        for (auto *list : {together, separate})
            for (QTreeWidgetItemIterator it(list); *it; ++it) {
                auto *item = *it;
                if (!item->childCount() && contains(itemZones(item), index)) {
                    item->setSelected(true);
                    list->scrollToItem(item);
                    updateSelection();
                    return;
                }
            }
    };
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rebuild();
    if (exec() != QDialog::Accepted)
        return false;
    edits_ = std::move(change.edits);
    return true;
}
} // namespace bending::ui
