/***************************************************************************
                          qgscollapsiblegroupbox.h
                             -------------------
    begin                : August 2012
    copyright            : (C) 2012 by Etienne Tourigny
    email                : etourigny dot dev at gmail dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#pragma once

#include "common/common_pch.h"

#include <QGroupBox>
#include <QPointer>
#include <QToolButton>

class QMouseEvent;
class QToolButton;
class QScrollArea;

namespace mtx::gui::Util {

/**
 * \ingroup gui
 * \class QgsGroupBoxCollapseButton
 */
class QgsGroupBoxCollapseButton : public QToolButton
{
    Q_OBJECT

  public:
    QgsGroupBoxCollapseButton( QWidget *parent = nullptr )
      : QToolButton( parent )
    {}

    bool altDown() const { return mAltDown; }
    void setAltDown( bool updown ) { mAltDown = updown; }

    bool shiftDown() const { return mShiftDown; }
    void setShiftDown( bool shiftdown ) { mShiftDown = shiftdown; }

  protected:
    void mouseReleaseEvent( QMouseEvent *event ) override;

  private:
    bool mAltDown = false;
    bool mShiftDown = false;
};

/**
 * \ingroup gui
 * \class QgsCollapsibleGroupBoxBasic
 * \brief A groupbox that collapses/expands when toggled.
 * Basic class QgsCollapsibleGroupBoxBasic does not auto-save collapsed or checked state
 * Holding Alt modifier key when toggling collapsed state will synchronize the toggling across other collapsible group boxes with the same syncGroup QString value
 * Holding Shift modifier key when attempting to toggle collapsed state will expand current group box, then collapse any others with the same syncGroup QString value
 * \note To add Collapsible properties in promoted QtDesigner widgets, you can add the following "Dynamic properties" by clicking on the green + in the propreties palette:
 * bool collapsed, QString syncGroup, bool scrollOnExpand
 */

class QgsCollapsibleGroupBoxBasic : public QGroupBox
{
    Q_OBJECT

    /**
     * The collapsed state of this group box. If it is set to TRUE, all content is hidden
     * if it is set to FALSE all content is shown.
     */
    Q_PROPERTY( bool collapsed READ isCollapsed WRITE setCollapsed USER true )

    /**
     * An optional group to be collapsed and uncollapsed in sync with this group box if the Alt-modifier
     * is pressed while collapsing / uncollapsing.
     */
    Q_PROPERTY( QString syncGroup READ syncGroup WRITE setSyncGroup )

    /**
     * If this property is set to TRUE, a parent scroll area will try to make sure that the whole
     * group box is visible when uncollapsing it.
     */
    Q_PROPERTY( bool scrollOnExpand READ scrollOnExpand WRITE setScrollOnExpand )

  public:
    QgsCollapsibleGroupBoxBasic( QWidget *parent = nullptr );
    QgsCollapsibleGroupBoxBasic( const QString &title, QWidget *parent = nullptr );

    /**
     * Returns the current collapsed state of this group box
     */
    bool isCollapsed() const { return mCollapsed; }

    /**
     * Collapse or uncollapse this groupbox
     *
     * \param collapse Will collapse on TRUE and uncollapse on FALSE
     */
    void setCollapsed( bool collapse );

    /**
     * Named group which synchronizes collapsing action when triangle is clicked while holding alt modifier key
     */
    QString syncGroup() const { return mSyncGroup; }

    /**
     * Named group which synchronizes collapsing action when triangle is clicked while holding alt modifier key
     */
    void setSyncGroup( const QString &grp );

    //! Sets this to FALSE to not automatically scroll parent QScrollArea to this widget's contents when expanded
    void setScrollOnExpand( bool scroll ) { mScrollOnExpand = scroll; }

    //! If this is set to FALSE the parent QScrollArea will not be automatically scrolled to this widget's contents when expanded
    bool scrollOnExpand() {return mScrollOnExpand;}

  Q_SIGNALS:
    //! Signal emitted when groupbox collapsed/expanded state is changed, and when first shown
    void collapsedStateChanged( bool collapsed );

  public Q_SLOTS:
    void checkToggled( bool ckd );
    void checkClicked( bool ckd );
    void toggleCollapsed();

    /**
     * Overridden to prepare base call and avoid crash due to specific QT versions
     *
     * \since QGIS 3.16
     */
    void setStyleSheet( const QString &style );

  protected:
    void init();

    //! Visual fixes for when group box is collapsed/expanded
    void collapseExpandFixes();

    void showEvent( QShowEvent *event ) override;
    void mousePressEvent( QMouseEvent *event ) override;
    void mouseReleaseEvent( QMouseEvent *event ) override;
    void changeEvent( QEvent *event ) override;

    void updateStyle();
    QRect titleRect() const;
    void clearModifiers();

    bool mCollapsed;
    bool mInitFlat;
    bool mInitFlatChecked;
    bool mScrollOnExpand;
    bool mShown;
    QScrollArea *mParentScrollArea = nullptr;
    QgsGroupBoxCollapseButton *mCollapseButton = nullptr;
    QWidget *mSyncParent = nullptr;
    QString mSyncGroup;
    bool mAltDown;
    bool mShiftDown;
    bool mTitleClicked;

    QIcon mCollapseIcon;
    QIcon mExpandIcon;
};

/**
 * \ingroup gui
 * \class QgsCollapsibleGroupBox
 * \brief A groupbox that collapses/expands when toggled and can save its collapsed and checked states.
 * By default, it auto-saves only its collapsed state to the global settings based on the widget and it's parent names.
 * Holding Alt modifier key when toggling collapsed state will synchronize the toggling across other collapsible group boxes with the same syncGroup QString value
 * Holding Shift modifier key when attempting to toggle collapsed state will expand current group box, then collapse any others with the same syncGroup QString value
 * \see basic class QgsCollapsibleGroupBoxBasic which does not auto-save states
 * \note To add Collapsible properties in promoted QtDesigner widgets, you can add the following "Dynamic properties" by clicking on the green + in the properties palette:
 * bool collapsed, bool saveCollapsedState, bool saveCheckedState, QString syncGroup
 */

class QgsCollapsibleGroupBox : public QgsCollapsibleGroupBoxBasic
{
    Q_OBJECT

    /**
     * Shall the collapsed state of this group box be saved and loaded persistently in settings
     */
    Q_PROPERTY( bool saveCollapsedState READ saveCollapsedState WRITE setSaveCollapsedState )

    /**
     * Shall the checked state of this group box be saved and loaded persistently in settings
     */
    Q_PROPERTY( bool saveCheckedState READ saveCheckedState WRITE setSaveCheckedState )

  public:
    QgsCollapsibleGroupBox( QWidget *parent = nullptr );
    QgsCollapsibleGroupBox( const QString &title, QWidget *parent = nullptr );
    ~QgsCollapsibleGroupBox() override;

    //! Sets this to FALSE to not save/restore collapsed state
    void setSaveCollapsedState( bool save ) { mSaveCollapsedState = save; }

    /**
     * Set this to TRUE to save/restore checked state
     * \note only turn on mSaveCheckedState for groupboxes NOT used
     * in multiple places or used as options for different parent objects
    */
    void setSaveCheckedState( bool save ) { mSaveCheckedState = save; }
    bool saveCollapsedState() { return mSaveCollapsedState; }
    bool saveCheckedState() { return mSaveCheckedState; }

    //! Sets this to a defined string to share save/restore states across different parent dialogs
    void setSettingGroup( const QString &group );
    //! Returns the name of the setting group in which the collapsed state will be saved
    QString settingGroup() const { return mSettingGroup; }

  public Q_SLOTS:

    /**
     * Will load the collapsed and checked state
     *
     * The configuration path from which it is loaded is defined by
     *
     * - The object name
     * - The settingGroup
     */
    void loadState();

    /**
     * Will save the collapsed and checked state
     *
     * The configuration path to which it is saved is defined by
     *
     * - The object name
     * - The settingGroup
     */
    void saveState() const;

  protected:
    void init();
    void showEvent( QShowEvent *event ) override;
    QString saveKey() const;

    bool mSaveCollapsedState;
    bool mSaveCheckedState;
    QString mSettingGroup;
};

} // namespace mtx::gui::Util
