#ifndef SWEEPCAP_RESOURCE_H
#define SWEEPCAP_RESOURCE_H

#define IDI_APPICON        101

// Tray menu commands
#define IDM_EXIT           40001
#define IDM_DUMP_GEOMETRY  40002
#define IDM_OPEN_FOLDER    40003
#define IDM_FOLDER_CHANGE  40004
#define IDM_FOLDER_DEFAULT 40005
#define IDM_RUN_AT_STARTUP 40006

// Command ranges for the radio submenus. The item id carries the index, so
// handling a click is one subtraction and no lookup table.
#define IDM_GESTURE_FIRST 40100
#define IDM_GESTURE_LAST  40119
#define IDM_GRID_FIRST    40120
#define IDM_GRID_LAST     40139

// User-visible strings.
//
// Every string the user can see lives in the string table, never as a literal
// in the code. Adding a language then means adding a STRINGTABLE block rather
// than touching the source.
#define IDS_TRAY_TIP       1001
#define IDS_MENU_EXIT      1002
#define IDS_MENU_DUMP      1003
#define IDS_ERR_TRAY       1004
#define IDS_ERR_HOOK       1005

#define IDS_MENU_OPEN_FOLDER    1010
#define IDS_MENU_GESTURE        1011
#define IDS_MENU_GRID           1012
#define IDS_MENU_FOLDER         1013
#define IDS_MENU_FOLDER_CHANGE  1014
#define IDS_MENU_FOLDER_DEFAULT 1015
#define IDS_MENU_STARTUP        1016
#define IDS_FOLDER_PICK_TITLE   1017
#define IDS_ERR_STARTUP         1018

// Shown when a capture could not be delivered. Which one depends on what
// still worked, so the user knows whether the capture is lost.
#define IDS_ERR_CAPTURE_TITLE 1020
#define IDS_ERR_SAVE_ONLY     1021
#define IDS_ERR_CLIP_ONLY     1022
#define IDS_ERR_DELIVERY_ALL  1023
#define IDS_ERR_CAPTURE_NONE  1024

// Format string for a division grid entry, e.g. "12 x 6 분할".
#define IDS_GRID_DIVISIONS    1025

// One label per gesture, in the order the menu lists them.
#define IDS_GESTURE_CTRL_ALT       1030
#define IDS_GESTURE_CTRL_WIN       1031
#define IDS_GESTURE_ALT_WIN        1032
#define IDS_GESTURE_CTRL_ALT_WIN   1033
#define IDS_GESTURE_CTRL_SHIFT_WIN 1034

#endif  // SWEEPCAP_RESOURCE_H
