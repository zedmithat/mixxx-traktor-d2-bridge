#pragma once

#include <QBasicTimer>
#include <QModelIndex>
#include <QTreeView>
#include <QStringList>

#include "library/library_decl.h"
#include "widget/wbasewidget.h"

class LibraryFeature;
class QPoint;

class WLibrarySidebar : public QTreeView, public WBaseWidget {
    Q_OBJECT
  public:
    explicit WLibrarySidebar(QWidget* parent = nullptr);

    void contextMenuEvent(QContextMenuEvent * event) override;
    void dragMoveEvent(QDragMoveEvent * event) override;
    void dragEnterEvent(QDragEnterEvent * event) override;
    void dropEvent(QDropEvent * event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void timerEvent(QTimerEvent* event) override;
    void toggleSelectedItem();
    bool isLeafNodeSelected();
    bool isChildIndexSelected(const QModelIndex& index);
    bool isFeatureRootIndexSelected(LibraryFeature* pFeature);
    QString d2SelectedItemLabel();
    QStringList d2VisibleLabels(int radius);
    int d2SelectedVisibleRow();
    int d2VisibleRowCount();
    bool d2ActivateVisibleLabel(const QString& label);
    bool d2OpenRemovableDevice(const QString& label);

  public slots:
    void selectIndex(const QModelIndex&);
    void selectChildIndex(const QModelIndex&, bool selectItem = true);
    void slotSetFont(const QFont& font);

  signals:
    void rightClicked(const QPoint&, const QModelIndex&);
    void renameItem(const QModelIndex&);
    void deleteItem(const QModelIndex&);
    FocusWidget setLibraryFocus(FocusWidget newFocus);

  protected:
    bool event(QEvent* pEvent) override;

  private:
    void focusSelectedIndex();
    QModelIndex selectedIndex();

    QBasicTimer m_expandTimer;
    QModelIndex m_hoverIndex;
};
