#include <iostream>
#include <string>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#define WIDTH 800
#define HEIGHT 600
#define ROW_HEIGHT 28
#define HEADER_Y 90
#define TABLE_START_Y 120
#define UPDATE_INTERVAL_MS 1000

// Data for a single process read from /proc
typedef struct ProcessEntry {
    int pid;
    std::string name;
    char state;
    double cpu_pct;
    uint64_t memory_kb;
    // For CPU calculation between updates
    uint64_t prev_utime;
    uint64_t prev_stime;
    uint64_t prev_total_cpu;
    uint64_t curr_utime;
    uint64_t curr_stime;
    uint64_t curr_total_cpu;
    bool seen_before;
} ProcessEntry;

typedef struct AppData {
    TTF_Font *font;
    TTF_Font *font_small;
    TTF_Font *font_title;
    SDL_Texture *proc_image;
    SDL_Texture *file_image;

    std::vector<ProcessEntry*> processes;

    // Sort column: 0=PID, 1=Name, 2=CPU%, 3=Memory
    int sort_column;
    // Sort direction: true=ascending, false=descending
    bool sort_ascending;

    // Scroll offset (pixels)
    int scroll_offset;

    // Selected process index (-1 = none)
    int selected_index;

    // System totals for summary bar
    double total_cpu_pct;
    uint64_t total_mem_kb;
    uint64_t used_mem_kb;
    int num_processes;

    // Timing
    uint32_t last_update;

    // Column header rects for click detection
    SDL_Rect col_pid;
    SDL_Rect col_name;
    SDL_Rect col_cpu;
    SDL_Rect col_mem;

    // Kill button rect
    SDL_Rect kill_btn;
    bool show_kill;
} AppData;

// Function declarations
void initialize(SDL_Renderer *renderer, AppData *data_ptr);
void handleEvent(SDL_Event *event, SDL_Renderer *renderer, AppData *data_ptr);
void update(AppData *data_ptr);
void render(SDL_Renderer *renderer, AppData *data_ptr);
void readProcesses(AppData *data_ptr);
uint64_t getTotalCpuTime();
void readMemInfo(uint64_t &total_kb, uint64_t &used_kb);
void sortProcesses(AppData *data_ptr);
SDL_Texture* renderText(SDL_Renderer *renderer, TTF_Font *font,
                        const char *text, SDL_Color color);
bool pointInRect(int x, int y, SDL_Rect& rect);
std::string formatMemory(uint64_t kb);
void quit(AppData *data_ptr);

int main(int argc, char *argv[])
{
    std::cout << "Task Manager" << std::endl;

    // Initialize SDL2
    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();

    // Create window and renderer
    SDL_Renderer *renderer;
    SDL_Window *window;
    SDL_CreateWindowAndRenderer(WIDTH, HEIGHT, 0, &window, &renderer);
    SDL_SetWindowTitle(window, "Task Manager");

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Initialize application
    AppData data;
    data.sort_column = 2;       // default sort by CPU%
    data.sort_ascending = false; // highest first
    data.scroll_offset = 0;
    data.selected_index = -1;
    data.total_cpu_pct = 0.0;
    data.total_mem_kb = 0;
    data.used_mem_kb = 0;
    data.num_processes = 0;
    data.last_update = 0;
    data.show_kill = false;
    initialize(renderer, &data);

    // Initial read
    readProcesses(&data);
    SDL_Delay(200);
    update(&data);

    // Render loop
    SDL_Event event;
    bool running = true;
    while (running)
    {
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
            {
                running = false;
            }
            else
            {
                handleEvent(&event, renderer, &data);
            }
        }

        // Update at fixed interval
        uint32_t now = SDL_GetTicks();
        if (now - data.last_update >= UPDATE_INTERVAL_MS)
        {
            update(&data);
            data.last_update = now;
        }

        render(renderer, &data);
        SDL_Delay(16);
    }

    // Clean up
    quit(&data);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();

    return 0;
}

void initialize(SDL_Renderer *renderer, AppData *data_ptr)
{
    // Load fonts
    data_ptr->font = TTF_OpenFont("resrc/fonts/OpenSans-Regular.ttf", 15);
    data_ptr->font_small = TTF_OpenFont("resrc/fonts/OpenSans-Regular.ttf", 12);
    data_ptr->font_title = TTF_OpenFont("resrc/fonts/OpenSans-Regular.ttf", 20);

    // Load icons (reuse from file browser resources)
    SDL_Surface *proc_surf = IMG_Load("resrc/images/file_icon.png");
    data_ptr->proc_image = SDL_CreateTextureFromSurface(renderer, proc_surf);
    SDL_FreeSurface(proc_surf);

    SDL_Surface *dir_surf = IMG_Load("resrc/images/directory_icon.png");
    data_ptr->file_image = SDL_CreateTextureFromSurface(renderer, dir_surf);
    SDL_FreeSurface(dir_surf);

    // Column header positions
    data_ptr->col_pid  = { 10,  HEADER_Y, 70,  ROW_HEIGHT };
    data_ptr->col_name = { 80,  HEADER_Y, 310, ROW_HEIGHT };
    data_ptr->col_cpu  = { 390, HEADER_Y, 160, ROW_HEIGHT };
    data_ptr->col_mem  = { 550, HEADER_Y, 240, ROW_HEIGHT };

    // Kill button (shown when a process is selected)
    data_ptr->kill_btn = { WIDTH - 110, 10, 95, 28 };
}

void handleEvent(SDL_Event *event, SDL_Renderer *renderer, AppData *data_ptr)
{
    if (event->type == SDL_MOUSEBUTTONDOWN && event->button.button == SDL_BUTTON_LEFT)
    {
        int x = event->button.x;
        int y = event->button.y;

        // Check column header clicks (to change sorting)
        if (pointInRect(x, y, data_ptr->col_pid))
        {
            if (data_ptr->sort_column == 0)
                data_ptr->sort_ascending = !data_ptr->sort_ascending;
            else
            {
                data_ptr->sort_column = 0;
                data_ptr->sort_ascending = true;
            }
            sortProcesses(data_ptr);
            return;
        }
        if (pointInRect(x, y, data_ptr->col_name))
        {
            if (data_ptr->sort_column == 1)
                data_ptr->sort_ascending = !data_ptr->sort_ascending;
            else
            {
                data_ptr->sort_column = 1;
                data_ptr->sort_ascending = true;
            }
            sortProcesses(data_ptr);
            return;
        }
        if (pointInRect(x, y, data_ptr->col_cpu))
        {
            if (data_ptr->sort_column == 2)
                data_ptr->sort_ascending = !data_ptr->sort_ascending;
            else
            {
                data_ptr->sort_column = 2;
                data_ptr->sort_ascending = false;
            }
            sortProcesses(data_ptr);
            return;
        }
        if (pointInRect(x, y, data_ptr->col_mem))
        {
            if (data_ptr->sort_column == 3)
                data_ptr->sort_ascending = !data_ptr->sort_ascending;
            else
            {
                data_ptr->sort_column = 3;
                data_ptr->sort_ascending = false;
            }
            sortProcesses(data_ptr);
            return;
        }

        // Check kill button
        if (data_ptr->show_kill && pointInRect(x, y, data_ptr->kill_btn))
        {
            if (data_ptr->selected_index >= 0 &&
                data_ptr->selected_index < (int)data_ptr->processes.size())
            {
                int pid = data_ptr->processes[data_ptr->selected_index]->pid;
                kill(pid, SIGTERM);
                data_ptr->selected_index = -1;
                data_ptr->show_kill = false;
            }
            return;
        }

        // Check if a row was clicked (select process)
        if (y >= TABLE_START_Y)
        {
            int row = (y - TABLE_START_Y - data_ptr->scroll_offset) / ROW_HEIGHT;
            if (row >= 0 && row < (int)data_ptr->processes.size())
            {
                if (data_ptr->selected_index == row)
                {
                    // Deselect if clicking same row
                    data_ptr->selected_index = -1;
                    data_ptr->show_kill = false;
                }
                else
                {
                    data_ptr->selected_index = row;
                    data_ptr->show_kill = true;
                }
            }
            return;
        }
    }
    else if (event->type == SDL_MOUSEWHEEL)
    {
        data_ptr->scroll_offset += 5 * event->wheel.y;

        // Clamp scroll
        if (data_ptr->scroll_offset > 0)
            data_ptr->scroll_offset = 0;

        int total_height = (int)data_ptr->processes.size() * ROW_HEIGHT;
        int visible_height = HEIGHT - TABLE_START_Y;
        int min_scroll = -(total_height - visible_height);
        if (min_scroll > 0) min_scroll = 0;
        if (data_ptr->scroll_offset < min_scroll)
            data_ptr->scroll_offset = min_scroll;
    }
}

void update(AppData *data_ptr)
{
    readProcesses(data_ptr);
    sortProcesses(data_ptr);

    // Read memory info
    readMemInfo(data_ptr->total_mem_kb, data_ptr->used_mem_kb);
    data_ptr->num_processes = (int)data_ptr->processes.size();

    // Compute total CPU usage
    double total = 0.0;
    for (int i = 0; i < (int)data_ptr->processes.size(); i++)
    {
        total += data_ptr->processes[i]->cpu_pct;
    }
    data_ptr->total_cpu_pct = total;

    // Fix selected index if list changed
    if (data_ptr->selected_index >= (int)data_ptr->processes.size())
    {
        data_ptr->selected_index = -1;
        data_ptr->show_kill = false;
    }
}

void render(SDL_Renderer *renderer, AppData *data_ptr)
{
    SDL_Color white = { 230, 230, 230, 255 };
    SDL_Color gray = { 150, 150, 165, 255 };
    SDL_Color header_color = { 180, 190, 210, 255 };

    // Clear background
    SDL_SetRenderDrawColor(renderer, 25, 25, 35, 255);
    SDL_RenderClear(renderer);

    // ---- Header area ----
    SDL_Rect header_bg = { 0, 0, WIDTH, 85 };
    SDL_SetRenderDrawColor(renderer, 35, 35, 50, 255);
    SDL_RenderFillRect(renderer, &header_bg);

    // Title with icon
    SDL_Rect icon_pos = { 10, 10, 22, 22 };
    SDL_RenderCopy(renderer, data_ptr->file_image, NULL, &icon_pos);

    SDL_Texture *title_tex = renderText(renderer, data_ptr->font_title,
                                         "Task Manager", white);
    SDL_Rect title_pos;
    SDL_QueryTexture(title_tex, NULL, NULL, &title_pos.w, &title_pos.h);
    title_pos.x = 38;
    title_pos.y = 8;
    SDL_RenderCopy(renderer, title_tex, NULL, &title_pos);
    SDL_DestroyTexture(title_tex);

    // Process count
    char count_str[64];
    snprintf(count_str, 64, "%d processes", data_ptr->num_processes);
    SDL_Texture *count_tex = renderText(renderer, data_ptr->font_small, count_str, gray);
    SDL_Rect count_pos;
    SDL_QueryTexture(count_tex, NULL, NULL, &count_pos.w, &count_pos.h);
    count_pos.x = 10;
    count_pos.y = 38;
    SDL_RenderCopy(renderer, count_tex, NULL, &count_pos);
    SDL_DestroyTexture(count_tex);

    // CPU usage bar
    int bar_x = 140;
    int bar_w = 200;
    int bar_h = 14;

    // CPU label
    SDL_Texture *cpu_label = renderText(renderer, data_ptr->font_small, "CPU", gray);
    SDL_Rect cpu_label_pos;
    SDL_QueryTexture(cpu_label, NULL, NULL, &cpu_label_pos.w, &cpu_label_pos.h);
    cpu_label_pos.x = bar_x - 30;
    cpu_label_pos.y = 37;
    SDL_RenderCopy(renderer, cpu_label, NULL, &cpu_label_pos);
    SDL_DestroyTexture(cpu_label);

    // CPU bar background
    SDL_Rect cpu_bar_bg = { bar_x, 38, bar_w, bar_h };
    SDL_SetRenderDrawColor(renderer, 50, 50, 65, 255);
    SDL_RenderFillRect(renderer, &cpu_bar_bg);

    // CPU bar fill
    double cpu_fill_pct = data_ptr->total_cpu_pct / 100.0;
    if (cpu_fill_pct > 1.0) cpu_fill_pct = 1.0;
    int cpu_fill_w = (int)(cpu_fill_pct * bar_w);
    SDL_Rect cpu_fill = { bar_x, 38, cpu_fill_w, bar_h };
    if (cpu_fill_pct < 0.6)
        SDL_SetRenderDrawColor(renderer, 66, 133, 244, 255);
    else if (cpu_fill_pct < 0.85)
        SDL_SetRenderDrawColor(renderer, 255, 193, 7, 255);
    else
        SDL_SetRenderDrawColor(renderer, 244, 67, 54, 255);
    SDL_RenderFillRect(renderer, &cpu_fill);
    SDL_SetRenderDrawColor(renderer, 70, 70, 90, 255);
    SDL_RenderDrawRect(renderer, &cpu_bar_bg);

    // CPU percentage text
    char cpu_pct_str[16];
    snprintf(cpu_pct_str, 16, "%.1f%%", data_ptr->total_cpu_pct);
    SDL_Texture *cpu_pct_tex = renderText(renderer, data_ptr->font_small,
                                           cpu_pct_str, white);
    SDL_Rect cpu_pct_pos;
    SDL_QueryTexture(cpu_pct_tex, NULL, NULL, &cpu_pct_pos.w, &cpu_pct_pos.h);
    cpu_pct_pos.x = bar_x + bar_w + 8;
    cpu_pct_pos.y = 37;
    SDL_RenderCopy(renderer, cpu_pct_tex, NULL, &cpu_pct_pos);
    SDL_DestroyTexture(cpu_pct_tex);

    // Memory usage bar
    int mem_bar_x = 440;

    SDL_Texture *mem_label = renderText(renderer, data_ptr->font_small, "Mem", gray);
    SDL_Rect mem_label_pos;
    SDL_QueryTexture(mem_label, NULL, NULL, &mem_label_pos.w, &mem_label_pos.h);
    mem_label_pos.x = mem_bar_x - 35;
    mem_label_pos.y = 37;
    SDL_RenderCopy(renderer, mem_label, NULL, &mem_label_pos);
    SDL_DestroyTexture(mem_label);

    SDL_Rect mem_bar_bg = { mem_bar_x, 38, bar_w, bar_h };
    SDL_SetRenderDrawColor(renderer, 50, 50, 65, 255);
    SDL_RenderFillRect(renderer, &mem_bar_bg);

    double mem_fill_pct = 0.0;
    if (data_ptr->total_mem_kb > 0)
        mem_fill_pct = (double)data_ptr->used_mem_kb / (double)data_ptr->total_mem_kb;
    int mem_fill_w = (int)(mem_fill_pct * bar_w);
    SDL_Rect mem_fill = { mem_bar_x, 38, mem_fill_w, bar_h };
    if (mem_fill_pct < 0.6)
        SDL_SetRenderDrawColor(renderer, 76, 175, 80, 255);
    else if (mem_fill_pct < 0.85)
        SDL_SetRenderDrawColor(renderer, 255, 193, 7, 255);
    else
        SDL_SetRenderDrawColor(renderer, 244, 67, 54, 255);
    SDL_RenderFillRect(renderer, &mem_fill);
    SDL_SetRenderDrawColor(renderer, 70, 70, 90, 255);
    SDL_RenderDrawRect(renderer, &mem_bar_bg);

    // Memory text
    char mem_str[64];
    snprintf(mem_str, 64, "%s / %s",
             formatMemory(data_ptr->used_mem_kb).c_str(),
             formatMemory(data_ptr->total_mem_kb).c_str());
    SDL_Texture *mem_tex = renderText(renderer, data_ptr->font_small, mem_str, white);
    SDL_Rect mem_pos;
    SDL_QueryTexture(mem_tex, NULL, NULL, &mem_pos.w, &mem_pos.h);
    mem_pos.x = mem_bar_x + bar_w + 8;
    mem_pos.y = 37;
    SDL_RenderCopy(renderer, mem_tex, NULL, &mem_pos);
    SDL_DestroyTexture(mem_tex);

    // Kill button (only when a process is selected)
    if (data_ptr->show_kill)
    {
        SDL_SetRenderDrawColor(renderer, 180, 40, 40, 255);
        SDL_RenderFillRect(renderer, &(data_ptr->kill_btn));
        SDL_SetRenderDrawColor(renderer, 220, 60, 60, 255);
        SDL_RenderDrawRect(renderer, &(data_ptr->kill_btn));
        SDL_Texture *kill_tex = renderText(renderer, data_ptr->font_small,
                                            "Kill Process", white);
        SDL_Rect kill_pos;
        SDL_QueryTexture(kill_tex, NULL, NULL, &kill_pos.w, &kill_pos.h);
        kill_pos.x = data_ptr->kill_btn.x +
                     (data_ptr->kill_btn.w - kill_pos.w) / 2;
        kill_pos.y = data_ptr->kill_btn.y +
                     (data_ptr->kill_btn.h - kill_pos.h) / 2;
        SDL_RenderCopy(renderer, kill_tex, NULL, &kill_pos);
        SDL_DestroyTexture(kill_tex);
    }

    // ---- Separator line ----
    SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
    SDL_RenderDrawLine(renderer, 0, 85, WIDTH, 85);

    // ---- Column headers ----
    SDL_Rect col_bg = { 0, HEADER_Y, WIDTH, ROW_HEIGHT };
    SDL_SetRenderDrawColor(renderer, 40, 40, 58, 255);
    SDL_RenderFillRect(renderer, &col_bg);

    // Draw sort indicator arrow for the active column
    const char *pid_label  = (data_ptr->sort_column == 0)
        ? (data_ptr->sort_ascending ? "PID ^" : "PID v") : "PID";
    const char *name_label = (data_ptr->sort_column == 1)
        ? (data_ptr->sort_ascending ? "Name ^" : "Name v") : "Name";
    const char *cpu_label_str = (data_ptr->sort_column == 2)
        ? (data_ptr->sort_ascending ? "CPU% ^" : "CPU% v") : "CPU%";
    const char *mem_label_str = (data_ptr->sort_column == 3)
        ? (data_ptr->sort_ascending ? "Memory ^" : "Memory v") : "Memory";

    SDL_Texture *h_pid = renderText(renderer, data_ptr->font, pid_label, header_color);
    SDL_Rect h_pid_pos;
    SDL_QueryTexture(h_pid, NULL, NULL, &h_pid_pos.w, &h_pid_pos.h);
    h_pid_pos.x = 15;
    h_pid_pos.y = HEADER_Y + 4;
    SDL_RenderCopy(renderer, h_pid, NULL, &h_pid_pos);
    SDL_DestroyTexture(h_pid);

    SDL_Texture *h_name = renderText(renderer, data_ptr->font, name_label, header_color);
    SDL_Rect h_name_pos;
    SDL_QueryTexture(h_name, NULL, NULL, &h_name_pos.w, &h_name_pos.h);
    h_name_pos.x = 85;
    h_name_pos.y = HEADER_Y + 4;
    SDL_RenderCopy(renderer, h_name, NULL, &h_name_pos);
    SDL_DestroyTexture(h_name);

    SDL_Texture *h_cpu = renderText(renderer, data_ptr->font, cpu_label_str, header_color);
    SDL_Rect h_cpu_pos;
    SDL_QueryTexture(h_cpu, NULL, NULL, &h_cpu_pos.w, &h_cpu_pos.h);
    h_cpu_pos.x = 395;
    h_cpu_pos.y = HEADER_Y + 4;
    SDL_RenderCopy(renderer, h_cpu, NULL, &h_cpu_pos);
    SDL_DestroyTexture(h_cpu);

    SDL_Texture *h_mem = renderText(renderer, data_ptr->font, mem_label_str, header_color);
    SDL_Rect h_mem_pos;
    SDL_QueryTexture(h_mem, NULL, NULL, &h_mem_pos.w, &h_mem_pos.h);
    h_mem_pos.x = 555;
    h_mem_pos.y = HEADER_Y + 4;
    SDL_RenderCopy(renderer, h_mem, NULL, &h_mem_pos);
    SDL_DestroyTexture(h_mem);

    // Header bottom line
    SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
    SDL_RenderDrawLine(renderer, 0, HEADER_Y + ROW_HEIGHT, WIDTH, HEADER_Y + ROW_HEIGHT);

    // ---- Process rows (scrollable) ----
    SDL_Rect clip = { 0, TABLE_START_Y, WIDTH, HEIGHT - TABLE_START_Y };
    SDL_RenderSetClipRect(renderer, &clip);

    for (int i = 0; i < (int)data_ptr->processes.size(); i++)
    {
        ProcessEntry *proc = data_ptr->processes[i];
        int y = TABLE_START_Y + (ROW_HEIGHT * i) + data_ptr->scroll_offset;

        // Skip if off screen
        if (y + ROW_HEIGHT < TABLE_START_Y || y > HEIGHT) continue;

        // Row background (alternate colors, highlight if selected)
        SDL_Rect row_bg = { 0, y, WIDTH, ROW_HEIGHT };
        if (i == data_ptr->selected_index)
        {
            SDL_SetRenderDrawColor(renderer, 55, 65, 95, 255);
        }
        else if (i % 2 == 0)
        {
            SDL_SetRenderDrawColor(renderer, 30, 30, 42, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer, 35, 35, 48, 255);
        }
        SDL_RenderFillRect(renderer, &row_bg);

        // Icon
        SDL_Rect proc_icon = { 85, y + 4, 18, 18 };
        SDL_RenderCopy(renderer, data_ptr->proc_image, NULL, &proc_icon);

        // PID
        char pid_str[16];
        snprintf(pid_str, 16, "%d", proc->pid);
        SDL_Texture *pid_tex = renderText(renderer, data_ptr->font_small,
                                           pid_str, gray);
        SDL_Rect pid_pos;
        SDL_QueryTexture(pid_tex, NULL, NULL, &pid_pos.w, &pid_pos.h);
        pid_pos.x = 15;
        pid_pos.y = y + 6;
        SDL_RenderCopy(renderer, pid_tex, NULL, &pid_pos);
        SDL_DestroyTexture(pid_tex);

        // Process name (truncate if long)
        std::string display_name = proc->name;
        if (display_name.length() > 30)
            display_name = display_name.substr(0, 27) + "...";
        SDL_Texture *name_tex = renderText(renderer, data_ptr->font_small,
                                            display_name.c_str(), white);
        SDL_Rect name_pos;
        SDL_QueryTexture(name_tex, NULL, NULL, &name_pos.w, &name_pos.h);
        name_pos.x = 108;
        name_pos.y = y + 6;
        SDL_RenderCopy(renderer, name_tex, NULL, &name_pos);
        SDL_DestroyTexture(name_tex);

        // CPU% bar + text
        int cpu_bar_x = 395;
        int cpu_bar_w = 100;
        int cpu_bar_h = 14;
        int cpu_bar_y = y + 7;

        SDL_Rect cpu_bg = { cpu_bar_x, cpu_bar_y, cpu_bar_w, cpu_bar_h };
        SDL_SetRenderDrawColor(renderer, 45, 45, 60, 255);
        SDL_RenderFillRect(renderer, &cpu_bg);

        double cpu_ratio = proc->cpu_pct / 100.0;
        if (cpu_ratio > 1.0) cpu_ratio = 1.0;
        int cpu_w = (int)(cpu_ratio * cpu_bar_w);
        if (cpu_w > 0)
        {
            SDL_Rect cpu_bar_fill = { cpu_bar_x, cpu_bar_y, cpu_w, cpu_bar_h };
            if (cpu_ratio < 0.3)
                SDL_SetRenderDrawColor(renderer, 66, 133, 244, 200);
            else if (cpu_ratio < 0.7)
                SDL_SetRenderDrawColor(renderer, 255, 193, 7, 200);
            else
                SDL_SetRenderDrawColor(renderer, 244, 67, 54, 200);
            SDL_RenderFillRect(renderer, &cpu_bar_fill);
        }
        SDL_SetRenderDrawColor(renderer, 60, 60, 75, 255);
        SDL_RenderDrawRect(renderer, &cpu_bg);

        char cpu_str[16];
        snprintf(cpu_str, 16, "%.1f%%", proc->cpu_pct);
        SDL_Texture *cpu_tex = renderText(renderer, data_ptr->font_small,
                                           cpu_str, white);
        SDL_Rect cpu_pos;
        SDL_QueryTexture(cpu_tex, NULL, NULL, &cpu_pos.w, &cpu_pos.h);
        cpu_pos.x = cpu_bar_x + cpu_bar_w + 6;
        cpu_pos.y = y + 6;
        SDL_RenderCopy(renderer, cpu_tex, NULL, &cpu_pos);
        SDL_DestroyTexture(cpu_tex);

        // Memory text
        std::string mem_str = formatMemory(proc->memory_kb);
        SDL_Texture *mem_tex = renderText(renderer, data_ptr->font_small,
                                           mem_str.c_str(), white);
        SDL_Rect mem_pos;
        SDL_QueryTexture(mem_tex, NULL, NULL, &mem_pos.w, &mem_pos.h);
        mem_pos.x = 555;
        mem_pos.y = y + 6;
        SDL_RenderCopy(renderer, mem_tex, NULL, &mem_pos);
        SDL_DestroyTexture(mem_tex);
    }

    SDL_RenderSetClipRect(renderer, NULL);

    SDL_RenderPresent(renderer);
}

void readProcesses(AppData *data_ptr)
{
    // Get total CPU time for percentage calculation
    uint64_t total_cpu = getTotalCpuTime();

    // Build a map of existing processes for CPU delta tracking
    // We'll mark which PIDs we've seen so we can remove dead ones
    std::vector<bool> still_alive(data_ptr->processes.size(), false);

    // Read /proc for all numeric directories (each is a PID)
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return;

    struct dirent *dir_entry;
    std::vector<ProcessEntry*> new_list;

    while ((dir_entry = readdir(proc_dir)) != NULL)
    {
        // Only numeric directory names are PIDs
        if (dir_entry->d_type != DT_DIR) continue;
        bool is_pid = true;
        for (int i = 0; dir_entry->d_name[i] != '\0'; i++)
        {
            if (dir_entry->d_name[i] < '0' || dir_entry->d_name[i] > '9')
            {
                is_pid = false;
                break;
            }
        }
        if (!is_pid) continue;

        int pid = atoi(dir_entry->d_name);

        // Read /proc/[pid]/stat
        char stat_path[64];
        snprintf(stat_path, 64, "/proc/%d/stat", pid);
        std::ifstream stat_file(stat_path);
        if (!stat_file.is_open()) continue;

        std::string stat_line;
        std::getline(stat_file, stat_line);
        stat_file.close();

        // Parse: pid (name) state ... utime stime ...
        // Name can contain spaces/parens, so find the last ')'
        size_t name_start = stat_line.find('(');
        size_t name_end = stat_line.rfind(')');
        if (name_start == std::string::npos || name_end == std::string::npos)
            continue;

        std::string name = stat_line.substr(name_start + 1,
                                             name_end - name_start - 1);
        // Fields after the closing paren
        std::string rest = stat_line.substr(name_end + 2);
        std::istringstream iss(rest);

        char state;
        int ppid, pgrp, session, tty_nr, tpgid;
        unsigned int flags;
        uint64_t minflt, cminflt, majflt, cmajflt, utime, stime;

        iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid
            >> flags >> minflt >> cminflt >> majflt >> cmajflt
            >> utime >> stime;

        // Read memory from /proc/[pid]/status (VmRSS line)
        char status_path[64];
        snprintf(status_path, 64, "/proc/%d/status", pid);
        std::ifstream status_file(status_path);
        uint64_t memory_kb = 0;
        if (status_file.is_open())
        {
            std::string sline;
            while (std::getline(status_file, sline))
            {
                if (sline.substr(0, 6) == "VmRSS:")
                {
                    std::istringstream mem_iss(sline.substr(6));
                    mem_iss >> memory_kb;
                    break;
                }
            }
            status_file.close();
        }

        // Look for existing entry to compute CPU delta
        ProcessEntry *proc = NULL;
        for (int i = 0; i < (int)data_ptr->processes.size(); i++)
        {
            if (data_ptr->processes[i]->pid == pid)
            {
                proc = data_ptr->processes[i];
                still_alive[i] = true;
                break;
            }
        }

        if (proc == NULL)
        {
            proc = new ProcessEntry();
            proc->pid = pid;
            proc->prev_utime = utime;
            proc->prev_stime = stime;
            proc->prev_total_cpu = total_cpu;
            proc->cpu_pct = 0.0;
            proc->seen_before = false;
        }
        else
        {
            proc->seen_before = true;
        }

        proc->name = name;
        proc->state = state;
        proc->memory_kb = memory_kb;

        // Update CPU times
        proc->curr_utime = utime;
        proc->curr_stime = stime;
        proc->curr_total_cpu = total_cpu;

        // Compute CPU percentage
        if (proc->seen_before)
        {
            uint64_t proc_delta = (proc->curr_utime + proc->curr_stime)
                                - (proc->prev_utime + proc->prev_stime);
            uint64_t total_delta = proc->curr_total_cpu - proc->prev_total_cpu;
            if (total_delta > 0)
            {
                proc->cpu_pct = 100.0 * (double)proc_delta / (double)total_delta;
            }
        }

        // Shift current to previous for next update
        proc->prev_utime = proc->curr_utime;
        proc->prev_stime = proc->curr_stime;
        proc->prev_total_cpu = proc->curr_total_cpu;
        proc->seen_before = true;

        new_list.push_back(proc);
    }
    closedir(proc_dir);

    // Delete processes that no longer exist
    for (int i = 0; i < (int)data_ptr->processes.size(); i++)
    {
        if (!still_alive[i])
        {
            // Check if it was moved to new_list
            bool found = false;
            for (int j = 0; j < (int)new_list.size(); j++)
            {
                if (new_list[j] == data_ptr->processes[i])
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                delete data_ptr->processes[i];
            }
        }
    }

    data_ptr->processes = new_list;
}

uint64_t getTotalCpuTime()
{
    std::ifstream proc_stat("/proc/stat");
    std::string line;
    std::getline(proc_stat, line);
    proc_stat.close();

    // "cpu  user nice system idle iowait irq softirq steal"
    std::istringstream iss(line);
    std::string label;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    iss >> label >> user >> nice >> system >> idle >> iowait
        >> irq >> softirq >> steal;

    return user + nice + system + idle + iowait + irq + softirq + steal;
}

void readMemInfo(uint64_t &total_kb, uint64_t &used_kb)
{
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    uint64_t mem_total = 0;
    uint64_t mem_available = 0;

    while (std::getline(meminfo, line))
    {
        if (line.substr(0, 9) == "MemTotal:")
        {
            std::istringstream iss(line.substr(9));
            iss >> mem_total;
        }
        else if (line.substr(0, 13) == "MemAvailable:")
        {
            std::istringstream iss(line.substr(13));
            iss >> mem_available;
        }
    }
    meminfo.close();

    total_kb = mem_total;
    used_kb = mem_total - mem_available;
}

void sortProcesses(AppData *data_ptr)
{
    bool asc = data_ptr->sort_ascending;
    int col = data_ptr->sort_column;

    std::sort(data_ptr->processes.begin(), data_ptr->processes.end(),
        [col, asc](const ProcessEntry *a, const ProcessEntry *b) {
            switch (col)
            {
                case 0: // PID
                    return asc ? a->pid < b->pid : a->pid > b->pid;
                case 1: // Name
                {
                    std::string a_lower = a->name;
                    std::string b_lower = b->name;
                    std::transform(a_lower.begin(), a_lower.end(),
                                   a_lower.begin(), ::tolower);
                    std::transform(b_lower.begin(), b_lower.end(),
                                   b_lower.begin(), ::tolower);
                    return asc ? a_lower < b_lower : a_lower > b_lower;
                }
                case 2: // CPU%
                    return asc ? a->cpu_pct < b->cpu_pct
                               : a->cpu_pct > b->cpu_pct;
                case 3: // Memory
                    return asc ? a->memory_kb < b->memory_kb
                               : a->memory_kb > b->memory_kb;
                default:
                    return false;
            }
        });
}

SDL_Texture* renderText(SDL_Renderer *renderer, TTF_Font *font,
                        const char *text, SDL_Color color)
{
    SDL_Surface *surface = TTF_RenderText_Blended(font, text, color);
    if (!surface)
    {
        surface = SDL_CreateRGBSurface(0, 1, 1, 32, 0, 0, 0, 0);
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

bool pointInRect(int x, int y, SDL_Rect& rect)
{
    if (x > rect.x && x < rect.x + rect.w &&
        y > rect.y && y < rect.y + rect.h)
    {
        return true;
    }
    return false;
}

std::string formatMemory(uint64_t kb)
{
    char buf[32];
    if (kb >= 1024 * 1024)
    {
        snprintf(buf, 32, "%.1f GB", (double)kb / (1024.0 * 1024.0));
    }
    else if (kb >= 1024)
    {
        snprintf(buf, 32, "%.1f MB", (double)kb / 1024.0);
    }
    else
    {
        snprintf(buf, 32, "%lu KB", kb);
    }
    return std::string(buf);
}

void quit(AppData *data_ptr)
{
    for (int i = 0; i < (int)data_ptr->processes.size(); i++)
    {
        delete data_ptr->processes[i];
    }
    data_ptr->processes.clear();

    SDL_DestroyTexture(data_ptr->proc_image);
    SDL_DestroyTexture(data_ptr->file_image);
    TTF_CloseFont(data_ptr->font);
    TTF_CloseFont(data_ptr->font_small);
    TTF_CloseFont(data_ptr->font_title);
}