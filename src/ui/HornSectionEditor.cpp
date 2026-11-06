#include "HornSectionEditor.h"
#include <QBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
namespace bending::ui {
namespace {
class HornDiagram final : public QWidget {
  public:
    core::HornRelief relief{27, 27, 9};
    double length = 100, profileHeight = 130;
    bool right = false, confirmed = false;
    int active = 0;
    std::function<void(int)> select;
    std::array<QRectF, 3> targets;
    explicit HornDiagram(QWidget *parent) : QWidget(parent) {
        setObjectName("hornDiagram");
        setMinimumSize(240, 230);
        setAccessibleName(QStringLiteral("Продольная схема сапожка"));
        setMouseTracking(true);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton)
            for (std::size_t i = 0; i < targets.size(); ++i)
                if (targets[i].contains(event->position()) && select) {
                    select(static_cast<int>(i));
                    return;
                }
        QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        const bool hit = std::any_of(targets.begin(), targets.end(),
                                     [&](const auto &r) { return r.contains(event->position()); });
        setCursor(hit ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor ink = palette().color(QPalette::Text), muted = palette().color(QPalette::Mid),
                     accent = palette().color(QPalette::Highlight);
        const double scale = std::max(
            1e-6, std::min((width() - 140.) / length, (this->height() - 80.) / profileHeight));
        const double x0 = (width() - length * scale) / 2, y0 = this->height() - 46;
        const auto at = [&](double s, double h) {
            return QPointF(x0 + (right ? length - s : s) * scale, y0 - h * scale);
        };
        const double root = core::hornRootHeight(relief),
                     straight = std::max(0., relief.projectionMm - relief.radiusMm);
        QPainterPath body;
        body.moveTo(at(0, 0));
        body.lineTo(at(length, 0));
        body.lineTo(at(length, profileHeight));
        body.lineTo(at(relief.projectionMm, profileHeight));
        body.lineTo(at(relief.projectionMm, root));
        QPainterPath arc;
        arc.moveTo(at(relief.projectionMm, root));
        for (int i = 1; i <= 96; ++i) {
            const double s = relief.projectionMm + (straight - relief.projectionMm) * i / 96.;
            const double u = relief.projectionMm - s;
            const auto point = at(s, root - std::sqrt(std::max(0., u * (2 * relief.radiusMm - u))));
            body.lineTo(point);
            arc.lineTo(point);
        }
        body.lineTo(at(0, relief.tipHeightMm));
        body.closeSubpath();
        auto fill = palette().color(QPalette::Midlight);
        fill.setAlpha(confirmed ? 210 : 100);
        p.setPen(QPen(muted, 1.3, confirmed ? Qt::SolidLine : Qt::DashLine));
        p.setBrush(fill);
        p.drawPath(body);
        p.setPen(QPen(accent, 2));
        p.setBrush(Qt::NoBrush);
        if (active == 0)
            p.drawLine(at(0, 0), at(relief.projectionMm, 0));
        else if (active == 1)
            p.drawPath(arc);
        else
            p.drawLine(at(0, 0), at(0, relief.tipHeightMm));
        const auto dimension = [&](int index, QPointF a, QPointF b, const QString &label,
                                   QPointF text) {
            p.setPen(QPen(active == index ? accent : muted, active == index ? 1.5 : 1));
            p.drawLine(a, b);
            const auto arrow = [&](QPointF tip, QPointF tail) {
                const double angle = std::atan2(tip.y() - tail.y(), tip.x() - tail.x());
                QPolygonF triangle;
                triangle << tip
                         << tip - QPointF(6 * std::cos(angle) - 2 * std::sin(angle),
                                          6 * std::sin(angle) + 2 * std::cos(angle))
                         << tip - QPointF(6 * std::cos(angle) + 2 * std::sin(angle),
                                          6 * std::sin(angle) - 2 * std::cos(angle));
                p.setBrush(active == index ? accent : muted);
                p.drawPolygon(triangle);
            };
            arrow(a, b);
            arrow(b, a);
            const double w = p.fontMetrics().horizontalAdvance(label) + 14;
            targets[static_cast<std::size_t>(index)] =
                QRectF(text.x() - w / 2, text.y() - 14, w, 28);
            p.setPen(active == index ? QPen(accent) : QPen(Qt::NoPen));
            p.setBrush(active == index ? palette().color(QPalette::AlternateBase)
                                       : palette().color(QPalette::Base));
            p.drawRoundedRect(targets[static_cast<std::size_t>(index)], 3, 3);
            p.setPen(active == index ? accent : ink);
            p.drawText(targets[static_cast<std::size_t>(index)], Qt::AlignCenter, label);
        };
        const auto number = [](double v) { return QLocale().toString(v, 'g', 6); };
        const double dimY = y0 + 24;
        p.setPen(QPen(muted, 1));
        for (double s : {0., relief.projectionMm})
            p.drawLine(at(s, 0) + QPointF(0, 4), QPointF(at(s, 0).x(), dimY + 5));
        dimension(0, {std::min(at(0, 0).x(), at(relief.projectionMm, 0).x()), dimY},
                  {std::max(at(0, 0).x(), at(relief.projectionMm, 0).x()), dimY},
                  number(relief.projectionMm),
                  {(at(0, 0).x() + at(relief.projectionMm, 0).x()) / 2, dimY});
        const double hx = at(0, 0).x() + (right ? 16 : -16);
        p.setPen(QPen(muted, 1));
        p.drawLine(at(0, 0), {hx, y0});
        p.drawLine(at(0, relief.tipHeightMm), {hx, at(0, relief.tipHeightMm).y()});
        dimension(2, {hx, y0}, {hx, at(0, relief.tipHeightMm).y()}, number(relief.tipHeightMm),
                  {at(0, 0).x() + (right ? 44 : -44), (y0 + at(0, relief.tipHeightMm).y()) / 2});
        const double angle = std::acos(std::max(0., 1 - relief.projectionMm / relief.radiusMm));
        const auto target =
            at(relief.projectionMm - relief.radiusMm + relief.radiusMm * std::cos(angle / 2),
               root - relief.radiusMm * std::sin(angle / 2));
        const QPointF label(x0 + length * scale * (right ? .34 : .66),
                            std::max(55., y0 - root * scale * .5 - 25));
        dimension(1, label, target, QStringLiteral("R%1").arg(number(relief.radiusMm)), label);
    }
};
QString number(double v) {
    return QString::number(v, 'g', 17);
}
} // namespace
HornSectionEditor::HornSectionEditor(QWidget *parent) : QWidget(parent) {
    setObjectName("hornSectionEditor");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 0);
    title_ = new QLabel(this);
    layout->addWidget(title_);
    body_ = new QBoxLayout(QBoxLayout::LeftToRight);
    layout->addLayout(body_);
    auto *diagram = new HornDiagram(this);
    diagram_ = diagram;
    body_->addWidget(diagram, 1);
    auto *host = new QWidget(this);
    auto *form = new QFormLayout(host);
    host->setMinimumWidth(190);
    const std::array<QString, 3> names{QStringLiteral("Вылет сапожка"),
                                       QStringLiteral("Радиус выемки"),
                                       QStringLiteral("Высота носика")};
    const std::array<const char *, 3> ids{"hornProjection", "hornRadius", "hornTipHeight"};
    for (std::size_t i = 0; i < 3; ++i) {
        fields_[i] = new QLineEdit(host);
        fields_[i]->setObjectName(ids[i]);
        fields_[i]->setMaxLength(24);
        fields_[i]->setPlaceholderText(i == 2 ? QStringLiteral("9") : QStringLiteral("27"));
        fields_[i]->setAccessibleName(names[i] + QStringLiteral(", мм"));
        fields_[i]->setInputMethodHints(Qt::ImhFormattedNumbersOnly);
        auto *row = new QWidget(host);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(fields_[i]);
        h->addWidget(new QLabel(QStringLiteral("мм"), row));
        form->addRow(names[i], row);
        fields_[i]->installEventFilter(this);
        connect(fields_[i], &QLineEdit::textChanged, this, [this] {
            refresh();
            if (!loading_ && onEdited)
                onEdited();
        });
    }
    body_->addWidget(host);
    diagram->select = [this, diagram](int index) {
        diagram->active = index;
        diagram->update();
        fields_[static_cast<std::size_t>(index)]->setFocus();
        fields_[static_cast<std::size_t>(index)]->selectAll();
    };
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setObjectName("hornEditorStatus");
    status_->setMinimumHeight(fontMetrics().height() * 2 + 4);
    layout->addWidget(status_);
    preset_ = new QPushButton(QStringLiteral("Задать 27 / R27 / 9"), this);
    preset_->setObjectName("hornUseReference");
    layout->addWidget(preset_, 0, Qt::AlignLeft);
    connect(preset_, &QPushButton::clicked, this, [this] {
        loading_ = true;
        fields_[0]->setText("27");
        fields_[1]->setText("27");
        fields_[2]->setText("9");
        loading_ = false;
        refresh();
        if (onEdited)
            onEdited();
    });
}
QStringList HornSectionEditor::draft() const {
    return {fields_[0]->text(), fields_[1]->text(), fields_[2]->text()};
}
QString HornSectionEditor::read(const QStringList &text, double length, double height,
                                std::optional<core::HornRelief> &out) {
    out.reset();
    if (text.isEmpty() ||
        std::all_of(text.begin(), text.end(), [](const auto &s) { return s.trimmed().isEmpty(); }))
        return {};
    if (text.size() != 3)
        return QStringLiteral("Задайте три размера сапожка.");
    std::array<double, 3> values;
    static const QRegularExpression decimal(QStringLiteral("^\\d+(?:[.,]\\d*)?$"));
    for (std::size_t i = 0; i < 3; ++i) {
        auto s = text[static_cast<qsizetype>(i)].trimmed();
        bool ok = false;
        values[i] = QLocale::c().toDouble(QString(s).replace(',', '.'), &ok);
        if (!ok || !decimal.match(s).hasMatch() || !std::isfinite(values[i]))
            return QStringLiteral("Задайте три положительных размера сапожка.");
    }
    core::HornRelief h{values[0], values[1], values[2]};
    if (!core::validHornRelief(h, length))
        return QStringLiteral("Размеры должны быть положительными, вылет — меньше длины секции.");
    if (!std::isfinite(height) || height <= 0 || core::hornRootHeight(h) >= height - 1e-6)
        return QStringLiteral("Выемка выходит за высоту профиля.");
    out = h;
    return {};
}
void HornSectionEditor::setSection(double length, double height, core::SegmentShape shape,
                                   std::optional<core::HornRelief> relief, QStringList text) {
    length_ = length;
    height_ = height;
    shape_ = shape;
    title_->setText(shape == core::SegmentShape::RightHorn
                        ? QStringLiteral("Правый сапожок · продольный вид")
                        : QStringLiteral("Левый сапожок · продольный вид"));
    loading_ = true;
    if (text.isEmpty() && relief)
        text = {number(relief->projectionMm), number(relief->radiusMm),
                number(relief->tipHeightMm)};
    while (text.size() < 3)
        text.append(QString{});
    for (std::size_t i = 0; i < 3; ++i)
        fields_[i]->setText(text[static_cast<qsizetype>(i)]);
    loading_ = false;
    refresh();
}
void HornSectionEditor::refresh() {
    std::optional<core::HornRelief> h;
    const auto error = read(draft(), length_, height_, h);
    auto *diagram = static_cast<HornDiagram *>(diagram_);
    diagram->length = std::max(1., length_);
    diagram->profileHeight = height_ > 0 ? height_ : 130;
    diagram->right = shape_ == core::SegmentShape::RightHorn;
    diagram->confirmed = h.has_value();
    if (h)
        diagram->relief = *h;
    else
        diagram->relief = {std::min(27., diagram->length / 2),
                           std::min({27., diagram->length / 2, diagram->profileHeight / 4}),
                           std::min(9., diagram->profileHeight / 4)};
    diagram->update();
    status_->setText(!error.isEmpty() ? error
                     : h              ? QString{}
                         : QStringLiteral("Размеры выемки не заданы."));
    preset_->setEnabled(!h);
}
bool HornSectionEditor::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::FocusIn)
        for (std::size_t i = 0; i < fields_.size(); ++i)
            if (object == fields_[i]) {
                static_cast<HornDiagram *>(diagram_)->active = static_cast<int>(i);
                diagram_->update();
                break;
            }
    return QWidget::eventFilter(object, event);
}
void HornSectionEditor::resizeEvent(QResizeEvent *event) {
    body_->setDirection(width() < 540 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    QWidget::resizeEvent(event);
}
} // namespace bending::ui
