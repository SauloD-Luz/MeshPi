/*
 * meshpi-tui.c — ncurses TUI for MeshPi
 * Compile: gcc -o meshpi-tui meshpi-tui.c -lncurses
 * Usage: sudo ./meshpi-tui
 *   (or, if Wi-Fi detection works without sudo: ./meshpi-tui)
 *       (if sudo is needed, enter password through the terminal)
 *
 * Auto-detects script-mesh.sh location:
 *   - executable's own directory
 *   - current working directory (.)
 *   - compile-time SCRIPT_DIR fallback
 *
 * Auto-detects Wi-Fi interface on first run:
 *   - iw dev -> ip link -> iwconfig -> fallback: prompt user
 *
 * Functions:
 *   1. Find network     — scan active IBSS networks and mesh neighbors
 *   2. Setup network   — run script-mesh.sh with IP
 *   3. Restart network — tear down and recreate the mesh interface
 *   4. Monitor network — live panel with origins, ping, traffic
 */

#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>

#define SCRIPT_DIR  "."                            /* script directory (default: current) */
#define SCRIPT_PATH SCRIPT_DIR "/script-mesh.sh"

static char g_iface[64] = "";         /* detected Wi-Fi interface */
static char g_script_path[512] = "";  /* path to script-mesh.sh */

/* ---------- Utilities ---------- */

/* Execute a command and return output as an allocated string (up to 64 KB) */
static char *exec_cmd(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    FILE *fp = popen(buf, "r");
    if (!fp) return strdup("(error executing command)");

    char *out = malloc(65536);
    if (!out) { pclose(fp); return strdup("(out of memory)"); }
    size_t pos = 0;
    out[0] = '\0';

    while (fgets(out + pos, 65536 - pos, fp)) {
        pos = strlen(out);
        if (pos >= 65535) break;
    }
    int rc = pclose(fp);

    if (pos == 0) {
        snprintf(out, 128, "(command executed, empty output — exit %d)", rc);
    }
    return out;
}

/* Scrollable window for displaying long text */
static void show_output(const char *title, const char *text) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    WINDOW *win = newwin(rows - 2, cols - 2, 1, 1);
    keypad(win, TRUE);
    scrollok(win, TRUE);

    mvwprintw(win, 0, 1, "[ %s ]", title);

    int line = 2;
    const char *p = text;
    char tmp[1024];
    while (*p && line < 10000) {
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof tmp - 1) tmp[i++] = *p++;
        if (*p == '\n') p++;
        tmp[i] = '\0';

        if (i > 0) {
            mvwprintw(win, line++, 1, "%s", tmp);
        } else {
            line++;
        }
        if (line >= rows - 6) break;
    }

    mvwprintw(win, rows - 6, 1, "--- Press any key to go back ---");
    wrefresh(win);
    wgetch(win);
    delwin(win);
}

/* Input box for text entry */
static void input_box(WINDOW *parent, int y, int x, char *buf, int maxlen) {
    int pos = strlen(buf);
    curs_set(1);
    int ch;
    while ((ch = wgetch(parent)) != '\n' && ch != 27) {
        if (ch == KEY_BACKSPACE || ch == 127) {
            if (pos > 0) {
                buf[--pos] = '\0';
                mvwaddch(parent, y, x + pos, ' ');
                wmove(parent, y, x + pos);
            }
        } else if (pos < maxlen - 1) {
            buf[pos++] = ch;
            buf[pos] = '\0';
            mvwprintw(parent, y, x, "%s", buf);
        }
        wrefresh(parent);
    }
    curs_set(0);
}

/* ---------- Auto Wi-Fi interface detection ---------- */

static void detect_interface(void) {
    /* Strategy 1: iw dev (most reliable) */
    char *res = exec_cmd("iw dev 2>/dev/null | awk '/Interface/{print $2}' | head -1");
    if (res && strlen(res) > 2 && strstr(res, "error") == NULL) {
        /* Remove trailing newline */
        size_t len = strlen(res);
        while (len > 0 && (res[len-1] == '\n' || res[len-1] == ' ')) res[--len] = '\0';
        if (strlen(res) >= 3) {
            strncpy(g_iface, res, sizeof g_iface - 1);
            free(res);
            return;
        }
    }
    free(res);

    /* Strategy 2: ip link show (wl* / wlan* pattern) */
    res = exec_cmd("ip link show 2>/dev/null | grep -oE 'wl[^:]*' | head -1");
    if (res && strlen(res) > 2 && strstr(res, "error") == NULL) {
        size_t len = strlen(res);
        while (len > 0 && (res[len-1] == '\n' || res[len-1] == ' ')) res[--len] = '\0';
        if (strlen(res) >= 3) {
            strncpy(g_iface, res, sizeof g_iface - 1);
            free(res);
            return;
        }
    }
    free(res);

    /* Strategy 3: legacy iwconfig */
    res = exec_cmd("iwconfig 2>/dev/null | grep -m1 'IEEE' | awk '{print $1}'");
    if (res && strlen(res) > 2 && strstr(res, "error") == NULL) {
        size_t len = strlen(res);
        while (len > 0 && (res[len-1] == '\n' || res[len-1] == ' ')) res[--len] = '\0';
        if (strlen(res) >= 3) {
            strncpy(g_iface, res, sizeof g_iface - 1);
            free(res);
            return;
        }
    }
    free(res);

    /* Fallback: prompt user */
    WINDOW *win = newwin(12, 56, LINES/2 - 6, COLS/2 - 28);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "Wi-Fi interface not detected automatically.");
    mvwprintw(win, 2, 2, "To find it, run in another terminal:");
    mvwprintw(win, 3, 4, "iw dev");
    mvwprintw(win, 4, 4, "or  ip link show | grep wl");
    mvwprintw(win, 5, 4, "or  iwconfig | grep IEEE");
    mvwprintw(win, 7, 2, "Enter Wi-Fi interface name:");
    mvwprintw(win, 10, 2, "[Enter] confirm  [Esc] quit");
    wrefresh(win);

    char buf[32] = "";
    input_box(win, 8, 2, buf, 31);

    if (strlen(buf) >= 2) {
        strncpy(g_iface, buf, sizeof g_iface - 1);
    } else {
        /* Last resort */
        strcpy(g_iface, "wlan0");
    }

    delwin(win);
}

/* ---------- Menu functions ---------- */

static void find_network(void) {
    char *iw_scan, *bat_o, *iwconfig_out, *combined;

    iw_scan = exec_cmd("iw dev %s scan 2>/dev/null | grep -A 5 'IBSS\\\\|freq:\\\\|signal:\\\\|SSID:' | head -80", g_iface);
    bat_o = exec_cmd("batctl o 2>/dev/null | head -30");
    iwconfig_out = exec_cmd("iwconfig %s 2>/dev/null | grep -E 'Mode|ESSID|Cell|Frequency'", g_iface);

    combined = malloc(65536);
    if (!combined) {
        show_output("Error", "Out of memory");
        free(iw_scan); free(bat_o); free(iwconfig_out);
        return;
    }

    snprintf(combined, 65536,
        "--- Interface: %s ---\n%s\n\n"
        "--- Found IBSS networks ---\n%s\n\n"
        "--- Mesh neighbors (BATMAN) ---\n%s",
        g_iface, iwconfig_out, iw_scan, bat_o);

    show_output("Networks and Neighbors", combined);

    free(iw_scan);
    free(bat_o);
    free(iwconfig_out);
    free(combined);
}

static void setup_network(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    WINDOW *win = newwin(8, 55, rows / 2 - 4, cols / 2 - 27);
    box(win, 0, 0);
    keypad(win, TRUE);

    mvwprintw(win, 1, 2, "Configure MeshPi");
    mvwprintw(win, 2, 2, "Wi-Fi interface: %s", g_iface);
    mvwprintw(win, 3, 2, "Enter IP (e.g. 192.168.1.10):");
    mvwprintw(win, 6, 2, "[Enter] confirm  [Esc] cancel");
    wrefresh(win);

    char ip[32];
    memset(ip, 0, sizeof ip);
    input_box(win, 4, 2, ip, 30);

    if (ip[0] == '\0') {
        delwin(win);
        return;
    }

    delwin(win);

    if (strlen(ip) < 7) {
        show_output("Error", "Invalid IP. Configure manually.");
        return;
    }

    /* Confirm */
    win = newwin(9, 55, rows / 2 - 4, cols / 2 - 27);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "Confirm");
    mvwprintw(win, 3, 2, "IP: %s", ip);
    mvwprintw(win, 4, 2, "Interface: %s", g_iface);
    mvwprintw(win, 6, 2, "[y] Yes   [n] No");
    wrefresh(win);

    int confirm = wgetch(win);
    delwin(win);

    if (confirm != 'y' && confirm != 'Y') return;

    /* Execute script passing interface + IP */
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s %s %s 2>&1", g_script_path, g_iface, ip);
    char *output = exec_cmd("%s", cmd);
    show_output("Setup Result", output);
    free(output);
}

static void restart_network(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    WINDOW *win = newwin(7, 55, rows / 2 - 3, cols / 2 - 27);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "Restart Mesh Network");
    mvwprintw(win, 3, 2, "Recreate interface %s with a new IP.", g_iface);
    mvwprintw(win, 5, 2, "[y] Yes   [n] No");
    wrefresh(win);

    int ch = wgetch(win);
    delwin(win);

    if (ch != 'y' && ch != 'Y') return;

    /* Prompt for new IP */
    win = newwin(8, 55, rows / 2 - 4, cols / 2 - 27);
    box(win, 0, 0);
    keypad(win, TRUE);

    mvwprintw(win, 1, 2, "Reconfigure Network");
    mvwprintw(win, 2, 2, "Interface: %s", g_iface);
    mvwprintw(win, 3, 2, "IP to bring up after restart:");
    mvwprintw(win, 6, 2, "[Enter] confirm  [Esc] cancel");
    wrefresh(win);

    char ip[32];
    memset(ip, 0, sizeof ip);
    input_box(win, 4, 2, ip, 30);

    if (ip[0] == '\0') {
        delwin(win);
        return;
    }
    delwin(win);

    if (strlen(ip) < 7) {
        show_output("Error", "Invalid IP. Restart cancelled.");
        return;
    }

    /* Run the script — it handles cleanup internally */
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s %s %s", g_script_path, g_iface, ip);
    char *output = exec_cmd("sudo %s 2>&1", cmd);
    show_output("Restart Complete", output);
    free(output);
}

static void monitor_network(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    WINDOW *win = newwin(rows - 2, cols - 2, 1, 1);
    nodelay(win, TRUE);
    keypad(win, TRUE);

    char ping_target[32] = "";
    int ping_active = 0;
    int quit = 0;

    /* Prompt for ping IP */
    WINDOW *prompt = newwin(8, 55, rows / 2 - 4, cols / 2 - 27);
    box(prompt, 0, 0);
    keypad(prompt, TRUE);
    mvwprintw(prompt, 1, 2, "Mesh Monitor");
    mvwprintw(prompt, 2, 2, "Interface: %s", g_iface);
    mvwprintw(prompt, 3, 2, "IP to monitor (ping):");
    mvwprintw(prompt, 6, 2, "[Enter] confirm  [Esc] read-only mode");
    wrefresh(prompt);

    input_box(prompt, 4, 2, ping_target, 30);

    delwin(prompt);

    if (strlen(ping_target) >= 7) ping_active = 1;

    werase(win);
    box(win, 0, 0);

    while (!quit) {
        char *bat_o = NULL, *ping_res = NULL, *iw_info = NULL;

        iw_info = exec_cmd("iwconfig %s 2>/dev/null | grep -E 'Mode|ESSID|Cell|Frequency|Link Quality'", g_iface);
        bat_o = exec_cmd("batctl o 2>/dev/null | head -15");

        if (ping_active) {
            ping_res = exec_cmd("ping -c 1 -W 2 %s 2>/dev/null | grep -E 'bytes from|loss|rtt' | head -3", ping_target);
        }

        werase(win);
        box(win, 0, 0);

        int y = 1;
        mvwprintw(win, y++, 2, "== MESH MONITOR ==");
        mvwprintw(win, y++, 2, "Interface: %s", g_iface);
        y++;
        mvwprintw(win, y++, 2, "Wi-Fi:");
        if (iw_info) {
            char *line = strtok(iw_info, "\n");
            while (line && y < rows - 8) {
                mvwprintw(win, y++, 4, "%s", line);
                line = strtok(NULL, "\n");
            }
        }
        y++;
        mvwprintw(win, y++, 2, "Mesh neighbors (BATMAN):");
        if (bat_o) {
            char *line = strtok(bat_o, "\n");
            while (line && y < rows - 8) {
                mvwprintw(win, y++, 4, "%s", line);
                line = strtok(NULL, "\n");
            }
        }
        if (ping_active) {
            y++;
            mvwprintw(win, y++, 2, "Ping to %s:", ping_target);
            if (ping_res) {
                char *line = strtok(ping_res, "\n");
                while (line && y < rows - 4) {
                    mvwprintw(win, y++, 4, "%s", line);
                    line = strtok(NULL, "\n");
                }
            } else {
                mvwprintw(win, y++, 4, "(no response)");
            }
        }

        mvwprintw(win, rows - 4, 2, "[q] Quit  [p] New IP  [r] Skip IP  Updates every 2s");

        free(iw_info);
        free(bat_o);
        free(ping_res);

        wrefresh(win);

        /* Wait for key for 2 seconds */
        int timeout = 0;
        while (timeout < 20) {
            int key = wgetch(win);
            if (key == 'q' || key == 'Q') {
                quit = 1;
                break;
            } else if (key == 'p' || key == 'P') {
                WINDOW *p = newwin(5, 45, rows / 2 - 2, cols / 2 - 22);
                box(p, 0, 0);
                mvwprintw(p, 1, 2, "New ping IP:");
                memset(ping_target, 0, sizeof ping_target);
                input_box(p, 2, 2, ping_target, 30);
                delwin(p);
                ping_active = (strlen(ping_target) >= 7);
                break;
            } else if (key == 'r' || key == 'R') {
                ping_active = 0;
                ping_target[0] = '\0';
                break;
            }
            usleep(100000);
            timeout++;
        }
    }

    delwin(win);
}

/* ---------- Main menu ---------- */

static void draw_menu(WINDOW *menu_win, int highlight) {
    const char *items[] = {
        "1. Find network",
        "2. Setup network",
        "3. Restart network",
        "4. Monitor network",
        "5. Quit"
    };
    int n = 5;

    werase(menu_win);
    box(menu_win, 0, 0);

    mvwprintw(menu_win, 1, 2, "=== MeshPi — Control Interface ===");
    mvwprintw(menu_win, 2, 2, "BATMAN-adv over IBSS");
    mvwprintw(menu_win, 3, 2, "Wi-Fi interface: %s", g_iface[0] ? g_iface : "(detecting...)");

    for (int i = 0; i < n; i++) {
        if (i == highlight)
            wattron(menu_win, A_REVERSE);
        mvwprintw(menu_win, i + 5, 4, "%s", items[i]);
        wattroff(menu_win, A_REVERSE);
    }

    mvwprintw(menu_win, 11, 2, "Arrows: navigate   Enter: select   q: quit");
    wrefresh(menu_win);
}

int main(void) {
    /* Initialize ncurses */
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);

    /* Determine script-mesh.sh path: try executable directory, then CWD */
    {
        char self_path[256] = "";
        ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (len > 0) {
            self_path[len] = '\0';
            char *slash = strrchr(self_path, '/');
            if (slash) {
                *slash = '\0';
                snprintf(g_script_path, sizeof g_script_path, "%s/script-mesh.sh", self_path);
            }
        }
        /* If not resolved, try current directory */
        if (access(g_script_path, F_OK) != 0)
            snprintf(g_script_path, sizeof g_script_path, "%s/script-mesh.sh", SCRIPT_DIR);
        /* If still missing, fall back to compile-time SCRIPT_DIR */
        if (access(g_script_path, F_OK) != 0)
            snprintf(g_script_path, sizeof g_script_path, SCRIPT_PATH);
    }

    /* Detect Wi-Fi interface */
    detect_interface();

    /* Show detected interface */
    if (g_iface[0]) {
        WINDOW *win = newwin(5, 50, LINES/2 - 2, COLS/2 - 25);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "Detected Wi-Fi interface:");
        wattron(win, A_BOLD);
        mvwprintw(win, 2, 2, "  %s", g_iface);
        wattroff(win, A_BOLD);
        mvwprintw(win, 3, 2, "Press any key to continue...");
        wrefresh(win);
        wgetch(win);
        delwin(win);
    }

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    WINDOW *menu_win = newwin(13, 55, rows / 2 - 6, cols / 2 - 27);
    keypad(menu_win, TRUE);

    int highlight = 0;
    int quit = 0;

    while (!quit) {
        draw_menu(menu_win, highlight);
        int ch = wgetch(menu_win);

        switch (ch) {
            case KEY_UP:
                highlight = (highlight - 1 + 5) % 5;
                break;
            case KEY_DOWN:
                highlight = (highlight + 1) % 5;
                break;
            case '\n':
                switch (highlight) {
                    case 0: find_network();         break;
                    case 1: setup_network();        break;
                    case 2: restart_network();      break;
                    case 3: monitor_network();      break;
                    case 4: quit = 1;               break;
                }
                touchwin(stdscr);
                refresh();
                break;
            case 'q':
            case 'Q':
                quit = 1;
                break;
        }
    }

    delwin(menu_win);
    endwin();
    return 0;
}