#pragma once

#include <QHeaderView>
#include <QMap>
#include <QMenu>

#include "preferences/usersettings.h"
#include "proto/headers.pb.h"

class TrackModel;
class QAction;
class QCheckBox;
class QContextMenuEvent;
class QWidget;
class WTrackTableViewHeader;

// Thanks to StackOverflow http://stackoverflow.com/questions/1163030/qt-qtableview-and-horizontalheader-restorestate
// answer with this code snippet: http://codepad.org/2gPIMPYU
class HeaderViewState {
public:
    HeaderViewState() {}

    // Populate the object based on the provided live view.
    explicit HeaderViewState(const WTrackTableViewHeader& headers);

    // Populate from an existing protobuf, mostly for testing.
    explicit HeaderViewState(const mixxx::library::HeaderViewState& pb)
            : m_view_state(pb) { }

    // Populate the object with the serialized protobuf data provided.
    explicit HeaderViewState(const QString& base64serialized);

    // Returns a serialized protobuf of the current state.
    QString saveState() const;
    // Apply the state to the provided view. Columns which are not part of the
    // view's model are ignored, columns of the model which the state has no
    // record for are made visible so they can be discovered.
    void restoreState(WTrackTableViewHeader* pHeaders);

    // Drops the sort indicator. Used for the layout that is shared by all
    // views, where sorting remains a per-view setting.
    void clearSortIndicator();
    // Takes over the sort indicator of other, or drops it if other has none.
    void adoptSortIndicator(const HeaderViewState& other);
    // Appends every column of other that this state has no record for, right
    // behind the column it followed in other. Used to keep the columns of
    // views which have columns the saved view doesn't have.
    void mergeMissingColumns(const HeaderViewState& other);

    // returns false if no headers are listed to be shown.
    bool healthy() const {
        if (m_view_state.header_state_size() == 0) {
            return false;
        }
        for (int i = 0; i < m_view_state.header_state_size(); ++i) {
            if (!m_view_state.header_state(i).hidden()) {
                return true;
            }
        }
        return false;
    }

private:
    // Returns the index of the record of column name, or -1.
    int indexOfColumn(const QString& name) const;

    mixxx::library::HeaderViewState m_view_state;
};


class WTrackTableViewHeader : public QHeaderView {
    Q_OBJECT
  public:
    explicit WTrackTableViewHeader(Qt::Orientation orientation,
            UserSettingsPointer pConfig,
            QWidget* pParent = nullptr);

    void contextMenuEvent(QContextMenuEvent* event) override;
    void setModel(QAbstractItemModel* model) override;

    void saveHeaderState();
    void restoreHeaderState();
    void loadDefaultHeaderState();
    // Returns false if the header state is not stored in the database (on first time usage)
    bool hasPersistedHeaderState();

    int getWidthOfHiddenColumn(int column) const;

    // Sets the font and ensures the height is adjusted immediately.
    // From other units this has to be called via WTrackTableViewHeader, not horizontalHeader()
    // because it does not (and can not) override QWidget::setFont()
    void setFont(const QFont& font);
    // We could also catch font change via changeEvent() -> QEvent::FontChange
    // but that's called way too often with no-ops and causes hilarious effects.

    // These two track the hovered section which is considered in paintSection().
    // This essentially restores the qss `hover` style.
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void leaveEvent(QEvent* pEvent) override;

    // Required to set the preferred height with custom padding
    QSize sizeHint() const override;
    // Work around Qt6 paint bug with sort indicator
    void paintSection(QPainter* pPainter, const QRect& rect, int logicalIndex) const override;

  signals:
    void shuffle();

  private slots:
    void showOrHideColumn(int);

  private:
    int hiddenCount();
    void clearActions();
    TrackModel* getTrackModel();
    // Whether all views share one column layout instead of each kind of view
    // storing its own.
    bool sharedColumnLayoutEnabled() const;

    void setHeightForFont();

    const UserSettingsPointer m_pConfig;

    QMenu m_menu;
    QMap<int, QCheckBox*> m_columnCheckBoxes;

    int m_preferredHeight;
    QMap<int, int> m_hiddenColumnSizes;
    int m_hoveredSection;
    int m_previousHoveredSection;
};
