#ifndef SWEEPCAP_RESOURCE_H
#define SWEEPCAP_RESOURCE_H

#define IDI_APPICON        101

// Tray menu commands
#define IDM_EXIT          40001
#define IDM_DUMP_GEOMETRY 40002

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

#endif  // SWEEPCAP_RESOURCE_H
