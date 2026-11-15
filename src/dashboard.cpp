// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/dashboard.h"
#include "xboxone_native/title_catalog.h"
#include "file_path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
#include <system_error>

namespace xboxone::dashboard {
namespace fs = std::filesystem;
namespace {

std::string glyph(char32_t cp) { // Segoe MDL2 Assets private-use code points, UTF-8
    return {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
            static_cast<char>(0x80 | (cp & 0x3F))};
}

fs::path to_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string from_path(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }

constexpr std::uint32_t tile_palette[] = {0xFF107C10, 0xFF2D7D9A, 0xFF8E562E, 0xFF744DA9, 0xFFC30052,
                                          0xFF0063B1, 0xFF486860, 0xFFCA5010, 0xFF5D5A58, 0xFF038387};

std::uint32_t title_color(const console::Title& t) {
    const auto& c = t.background_color;
    if (c.size() == 7 && c[0] == '#') {
        char* end = nullptr;
        const auto rgb = std::strtoul(c.c_str() + 1, &end, 16);
        if (end && !*end) return 0xFF000000u | static_cast<std::uint32_t>(rgb);
    }
    return tile_palette[std::hash<std::string>{}(t.id) % std::size(tile_palette)];
}

const std::map<std::string, std::string>& portuguese() {
    static const std::map<std::string, std::string> table{
        {"Who's playing?", "Quem está jogando?"}, {"Add new", "Adicionar"}, {"Home", "Início"},
        {"My games & apps", "Meus jogos e apps"}, {"Settings", "Configurações"}, {"Add games", "Adicionar jogos"},
        {"Last played", "Jogado por último"}, {"Pins", "Fixados"}, {"Games", "Jogos"},
        {"Import folder", "Importar pasta"}, {"Rescan library", "Reexaminar biblioteca"},
        {"No games yet", "Nenhum jogo ainda"}, {"Choose a folder that contains extracted title packages", "Escolha uma pasta com pacotes de títulos extraídos"},
        {"Play", "Jogar"}, {"Return to game", "Voltar ao jogo"}, {"Check compatibility", "Verificar compatibilidade"},
        {"Pin to Home", "Fixar no Início"}, {"Unpin from Home", "Desafixar do Início"},
        {"Remove from library", "Remover da biblioteca"}, {"Profile", "Perfil"}, {"Display", "Vídeo"},
        {"Audio", "Áudio"}, {"Controller", "Controle"}, {"Language", "Idioma"}, {"Storage", "Armazenamento"},
        {"System", "Sistema"}, {"Name", "Nome"}, {"Avatar", "Avatar"}, {"Switch profile", "Trocar perfil"},
        {"Remove profile", "Remover perfil"}, {"Resolution", "Resolução"}, {"Full screen", "Tela cheia"},
        {"On", "Ligado"}, {"Off", "Desligado"}, {"Interface sounds", "Sons da interface"},
        {"Interface volume", "Volume da interface"}, {"Vibration", "Vibração"}, {"Stick dead zone", "Zona morta do analógico"},
        {"Connected controllers", "Controles conectados"}, {"Console data", "Dados do console"}, {"Open", "Abrir"},
        {"Save data", "Dados salvos"}, {"Add library folder", "Adicionar pasta à biblioteca"},
        {"Rescan all", "Reexaminar tudo"}, {"Title time limit", "Tempo limite do título"},
        {"None (supervised)", "Nenhum (supervisionado)"}, {"About", "Sobre"}, {"Exit xone-native", "Sair do xone-native"},
        {"Sign out", "Sair do perfil"}, {"Quit game", "Fechar jogo"}, {"Playing", "Jogando"},
        {"Press the Xbox button or Ctrl+Home for the Guide", "Pressione o botão Xbox ou Ctrl+Home para o Guia"},
        {"Ready", "Pronto"}, {"Blocked", "Bloqueado"}, {"Not checked", "Não verificado"}, {"Checking", "Verificando"},
        {"Cancel", "Cancelar"}, {"OK", "OK"}, {"Remove", "Remover"}, {"Exit", "Sair"}, {"New profile", "Novo perfil"},
        {"Rename profile", "Renomear perfil"}, {"Done", "Concluir"}, {"Space", "Espaço"}, {"Shift", "Maiúsc."},
        {"Delete", "Apagar"}, {"Rescan", "Reexaminar"}, {"Publisher", "Editora"}, {"Version", "Versão"},
        {"Package", "Pacote"}, {"Executable", "Executável"}, {"Compatibility", "Compatibilidade"},
        {"Last result", "Último resultado"}, {"Times played", "Vezes jogado"}, {"Never", "Nunca"},
        {"Select", "Selecionar"}, {"Back", "Voltar"}, {"Manage", "Gerenciar"}, {"Guide", "Guia"},
        {"Imported titles", "Títulos importados"}, {"No titles found", "Nenhum título encontrado"},
        {"Could not start", "Não foi possível iniciar"}, {"Quit", "Fechar"}, {"A title is running", "Um título está em execução"},
        {"Remove this profile? Its save data stays on disk.", "Remover este perfil? Os dados salvos permanecem no disco."},
        {"Remove this title from the library? Its files are not deleted.", "Remover este título da biblioteca? Os arquivos não são apagados."},
        {"Quitting closes the running title.", "Sair fecha o título em execução."}, {"Starting", "Iniciando"}, {"Closing", "Fechando"},
        {"Library folders", "Pastas da biblioteca"}, {"X: remove", "X: remover"}, {"minutes", "minutos"},
        {"Display settings could not be applied", "Não foi possível aplicar o vídeo"},
        {"Locally recreated dashboard. Not Xbox One SystemOS.", "Dashboard recriada localmente. Não é o SystemOS do Xbox One."},
        {"Console state", "Estado do console"}, {"Settings saved", "Configurações salvas"},
        {"The console state could not be saved", "Não foi possível salvar o estado do console"},
    };
    return table;
}

bool overlaps(int a0, int a1, int b0, int b1) { return a0 < b1 && b0 < a1; }

std::string short_hash(const std::string& id) { return id.substr(0, 12); }

std::string format_size(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    char text[32];
    std::snprintf(text, sizeof text, unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
    return text;
}

std::size_t utf8_length(const std::string& s) {
    return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

void pop_utf8(std::string& s) {
    while (!s.empty()) {
        const auto c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

void append_utf8(std::string& s, char32_t c) {
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        s += static_cast<char>(0xE0 | (c >> 12)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F));
    }
}

constexpr const char* keyboard_rows[] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-_."};
constexpr std::uint32_t resolutions[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr std::size_t max_name_length = 16;
} // namespace

Dashboard::Dashboard(fs::path root, fs::path runtime, Platform& platform, session::Host& session)
    : root_(std::move(root)), runtime_(std::move(runtime)), platform_(platform), session_(session) {}

std::string Dashboard::translate(const char* english) const {
    if (state.settings.language == "pt-BR") {
        const auto& table = portuguese();
        if (const auto found = table.find(english); found != table.end()) return found->second;
    }
    return english;
}

bool Dashboard::start(std::string& error) {
    if (!console::load(root_, state, error)) return false;
    if (!error.empty()) error_dialog(translate("Console state"), error);
    error.clear();
    std::string display_error;
    if (!platform_.apply_display(state.settings.width, state.settings.height, state.settings.fullscreen, display_error))
        error_dialog(translate("Display settings could not be applied"), display_error);
    page = Page::sign_in;
    if (console::find_profile(state, state.settings.last_profile)) focus_keys_[int(Page::sign_in)] = "profile:" + state.settings.last_profile;
    for (const auto& t : state.titles) if (t.readiness == console::Readiness::unchecked) check_queue_.push_back(t.id);
    ++serial;
    return true;
}

const console::Profile* Dashboard::profile() const {
    for (const auto& p : state.profiles) if (p.id == profile_id) return &p;
    return nullptr;
}

const console::Title* Dashboard::title(const std::string& id) const {
    for (const auto& t : state.titles) if (t.id == id) return &t;
    return nullptr;
}

std::vector<const console::Title*> Dashboard::sorted_titles() const {
    std::vector<const console::Title*> out;
    for (const auto& t : state.titles) out.push_back(&t);
    std::stable_sort(out.begin(), out.end(), [](const auto* a, const auto* b) {
        if (a->last_played != b->last_played) return a->last_played > b->last_played;
        return a->name < b->name;
    });
    return out;
}

std::string Dashboard::header() const {
    switch (page) {
        case Page::library: return translate("My games & apps");
        case Page::settings: return translate("Settings");
        case Page::keyboard: return translate(keyboard_purpose == "rename" ? "Rename profile" : "New profile");
        case Page::sign_in: return translate("Who's playing?");
        default: return {};
    }
}

std::string& Dashboard::focus_key() { return guide_open ? guide_focus : focus_keys_[static_cast<int>(page)]; }

const Item* Dashboard::focused(const std::vector<Item>& list) const {
    const auto& key = guide_open ? guide_focus : focus_keys_[static_cast<int>(page)];
    for (const auto& item : list) if (item.key == key) return &item;
    return list.empty() ? nullptr : &list.front();
}

std::vector<Item> Dashboard::items() const { return page_items(page); }

std::vector<Item> Dashboard::page_items(Page which) const {
    auto* self = const_cast<Dashboard*>(this); // closures run from input(), never from rendering
    std::vector<Item> out;
    const auto titles = sorted_titles();
    const auto title_tile = [&](const console::Title* t, Kind kind, Rect r) {
        Item item;
        item.key = "title:" + t->id;
        item.kind = kind;
        item.rect = r;
        item.label = t->name;
        item.detail = t->publisher;
        item.image = t->logo;
        item.color = title_color(*t);
        item.selected = t->id == session_title;
        const auto id = t->id;
        item.activate = [self, id] { self->launch(id); };
        item.manage = [self, id] { self->selected_title = id; self->go(Page::details); };
        return item;
    };
    const auto action = [&](std::string key, Kind kind, Rect r, std::string label, char32_t icon, std::function<void()> run) {
        Item item;
        item.key = std::move(key);
        item.kind = kind;
        item.rect = r;
        item.label = std::move(label);
        item.glyph = icon ? glyph(icon) : std::string{};
        item.color = 0xFF2B2B2B;
        item.activate = std::move(run);
        return item;
    };
    switch (which) {
    case Page::sign_in: {
        const int count = static_cast<int>(state.profiles.size()) + 1;
        int x = (1920 - (count * 260 - 20)) / 2;
        for (const auto& p : state.profiles) {
            Item item;
            item.key = "profile:" + p.id;
            item.kind = Kind::profile;
            item.rect = {x, 380, 240, 300};
            item.label = p.name;
            item.value = std::to_string(p.avatar);
            const auto id = p.id;
            item.activate = [self, id] { self->sign_in(id); };
            out.push_back(std::move(item));
            x += 260;
        }
        out.push_back(action("profile:new", Kind::profile, {x, 380, 240, 300}, translate("Add new"), 0xE710,
                             [self] { self->open_keyboard("create", {}); }));
        break;
    }
    case Page::home: {
        if (!titles.empty()) {
            auto hero = title_tile(titles.front(), Kind::hero, {96, 150, 852, 480});
            hero.detail = titles.front()->last_played ? translate("Last played") : titles.front()->publisher;
            out.push_back(std::move(hero));
        } else {
            out.push_back(action("home:add", Kind::hero, {96, 150, 852, 480}, translate("Add games"), 0xE710, [self] {
                std::filesystem::path folder;
                if (self->platform_.pick_folder(folder)) self->import_folder(folder);
            }));
            out.back().detail = translate("Choose a folder that contains extracted title packages");
        }
        const Rect grid[] = {{964, 150, 426, 232}, {1406, 150, 426, 232}, {964, 398, 426, 232}, {1406, 398, 426, 232}};
        std::size_t slot = 0;
        for (std::size_t i = 1; i < titles.size() && i <= 3; ++i) out.push_back(title_tile(titles[i], Kind::tile, grid[slot++]));
        out.push_back(action("home:library", Kind::action_tile, grid[slot], translate("My games & apps"), 0xE7FC,
                             [self] { self->go(Page::library); }));
        int index = 0;
        const auto next = [&] { Rect r{96 + (index % 8) * 220, 710 + (index / 8) * 220, 204, 204}; ++index; return r; };
        for (const auto* t : titles) if (t->pinned) out.push_back(title_tile(t, Kind::tile, next()));
        out.push_back(action("home:settings", Kind::action_tile, next(), translate("Settings"), 0xE713, [self] { self->go(Page::settings); }));
        out.push_back(action("home:import", Kind::action_tile, next(), translate("Add games"), 0xE8B7, [self] {
            std::filesystem::path folder;
            if (self->platform_.pick_folder(folder)) self->import_folder(folder);
        }));
        break;
    }
    case Page::library: {
        out.push_back(action("lib:games", Kind::menu, {96, 200, 360, 64}, translate("Games") + "  " + std::to_string(titles.size()), 0xE7FC,
                             [self] { self->notify(self->translate("My games & apps")); }));
        out.back().selected = true;
        out.push_back(action("lib:import", Kind::menu, {96, 272, 360, 64}, translate("Import folder"), 0xE8B7, [self] {
            std::filesystem::path folder;
            if (self->platform_.pick_folder(folder)) self->import_folder(folder);
        }));
        out.push_back(action("lib:rescan", Kind::menu, {96, 344, 360, 64}, translate("Rescan library"), 0xE72C, [self] { self->rescan(); }));
        if (titles.empty()) {
            out.push_back(action("lib:empty", Kind::hero, {520, 200, 1304, 400}, translate("No games yet"), 0xE8B7, [self] {
                std::filesystem::path folder;
                if (self->platform_.pick_folder(folder)) self->import_folder(folder);
            }));
            out.back().detail = translate("Choose a folder that contains extracted title packages");
        }
        for (std::size_t i = 0; i < titles.size(); ++i) {
            auto tile = title_tile(titles[i], Kind::tile, {520 + static_cast<int>(i % 5) * 260, 200 + static_cast<int>(i / 5) * 260, 244, 244});
            const auto* t = titles[i];
            tile.value = t->id == checking_title ? translate("Checking") :
                         t->readiness == console::Readiness::ready ? translate("Ready") :
                         t->readiness == console::Readiness::blocked ? translate("Blocked") : translate("Not checked");
            out.push_back(std::move(tile));
        }
        break;
    }
    case Page::details: {
        const auto* t = title(selected_title);
        if (!t) break;
        const auto id = t->id;
        const bool running = id == session_title;
        out.push_back(action("details:play", Kind::button, {540, 600, 460, 64}, translate(running ? "Return to game" : "Play"), 0xE768,
                             [self, id] { self->launch(id); }));
        out.push_back(action("details:check", Kind::button, {540, 672, 460, 64}, translate("Check compatibility"), 0xE73E,
                             [self, id] { self->check(id); }));
        out.push_back(action("details:pin", Kind::button, {540, 744, 460, 64}, translate(t->pinned ? "Unpin from Home" : "Pin to Home"),
                             t->pinned ? 0xE77A : 0xE718, [self, id] {
            if (auto* entry = console::find_title(self->state, id)) { entry->pinned = !entry->pinned; self->persist(); }
        }));
        out.push_back(action("details:remove", Kind::button, {540, 816, 460, 64}, translate("Remove from library"), 0xE74D, [self, id] {
            self->confirm(self->translate("Remove from library"), self->translate("Remove this title from the library? Its files are not deleted."),
                          self->translate("Remove"), [self, id] {
                if (id == self->session_title) { self->error_dialog(self->translate("A title is running"), self->translate("Quit game")); return; }
                std::erase_if(self->state.titles, [&](const auto& entry) { return entry.id == id; });
                std::erase(self->check_queue_, id);
                self->persist();
                self->back();
            });
        }));
        break;
    }
    case Page::settings: {
        static constexpr std::pair<const char*, char32_t> categories[] = {
            {"Profile", 0xE77B}, {"Display", 0xE7F4}, {"Audio", 0xE767}, {"Controller", 0xE7FC},
            {"Language", 0xE774}, {"Storage", 0xEDA2}, {"System", 0xE770}};
        for (int i = 0; i < 7; ++i) {
            out.push_back(action("cat:" + std::to_string(i), Kind::menu, {96, 200 + i * 72, 360, 64}, translate(categories[i].first),
                                 categories[i].second, [self, i] { self->settings_category = i; self->focus_key() = "opt:0"; ++self->serial; }));
            out.back().selected = i == settings_category;
        }
        auto options = settings_options();
        out.insert(out.end(), std::make_move_iterator(options.begin()), std::make_move_iterator(options.end()));
        break;
    }
    case Page::keyboard: {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 10; ++c) {
                char key = keyboard_rows[r][c];
                if (keyboard_shift && key >= 'a' && key <= 'z') key = static_cast<char>(key - 32);
                Item item;
                item.key = "key:" + std::to_string(r) + ":" + std::to_string(c);
                item.kind = Kind::key;
                item.rect = {444 + c * 104, 400 + r * 88, 96, 80};
                item.label = std::string(1, key);
                item.activate = [self, key] { self->text(static_cast<char32_t>(key)); };
                out.push_back(std::move(item));
            }
        out.push_back(action("key:shift", Kind::key, {444, 752, 200, 80}, translate("Shift"), 0xE752, [self] { self->keyboard_shift = !self->keyboard_shift; ++self->serial; }));
        out.back().selected = keyboard_shift;
        out.push_back(action("key:space", Kind::key, {652, 752, 408, 80}, translate("Space"), 0, [self] { self->text(U' '); }));
        out.push_back(action("key:delete", Kind::key, {1068, 752, 200, 80}, translate("Delete"), 0xE750, [self] { self->text(U'\b'); }));
        out.push_back(action("key:done", Kind::key, {1276, 752, 200, 80}, translate("Done"), 0xE73E, [self] { self->finish_keyboard(); }));
        for (auto& item : out) item.color = item.kind == Kind::key ? 0xFF333333 : item.color;
        break;
    }
    case Page::session: break; // The title owns this surface; system actions live in the Guide.
    }
    return out;
}

std::vector<Item> Dashboard::settings_options() const {
    auto* self = const_cast<Dashboard*>(this);
    std::vector<Item> out;
    int row = 0;
    const auto option = [&](std::string label, std::string value, std::function<void()> activate, std::function<void(int)> adjust = {}) {
        Item item;
        item.key = "opt:" + std::to_string(row);
        item.kind = Kind::option;
        item.rect = {520, 200 + row * 80, 1000, 72};
        item.label = std::move(label);
        item.value = std::move(value);
        item.activate = std::move(activate);
        item.adjust = std::move(adjust);
        ++row;
        out.push_back(std::move(item));
        return &out.back();
    };
    const auto on_off = [&](bool value) { return translate(value ? "On" : "Off"); };
    auto& s = self->state.settings;
    switch (settings_category) {
    case 0: {
        const auto* p = profile();
        if (!p) break;
        const auto id = p->id;
        option(translate("Name"), p->name, [self, name = p->name] { self->open_keyboard("rename", name); });
        const auto cycle = [self, id](int step) {
            if (auto* entry = console::find_profile(self->state, id)) { entry->avatar = (entry->avatar + 12 + step) % 12; self->persist(); }
        };
        option(translate("Avatar"), std::to_string(p->avatar + 1) + " / 12", [cycle] { cycle(1); }, cycle)->value = std::to_string(p->avatar);
        out.back().kind = Kind::option;
        out.back().detail = "avatar";
        option(translate("Switch profile"), {}, [self] { self->history_.clear(); self->go(Page::sign_in); self->history_.clear(); });
        option(translate("Remove profile"), {}, [self, id] {
            self->confirm(self->translate("Remove profile"), self->translate("Remove this profile? Its save data stays on disk."),
                          self->translate("Remove"), [self, id] {
                if (!self->session_title.empty()) { self->error_dialog(self->translate("A title is running"), self->translate("Quit game")); return; }
                std::erase_if(self->state.profiles, [&](const auto& entry) { return entry.id == id; });
                if (self->state.settings.last_profile == id) self->state.settings.last_profile.clear();
                self->profile_id.clear();
                self->persist();
                self->history_.clear();
                self->page = Page::sign_in;
                ++self->serial;
            });
        });
        break;
    }
    case 1: {
        std::size_t current = 2;
        for (std::size_t i = 0; i < std::size(resolutions); ++i)
            if (resolutions[i][0] == s.width && resolutions[i][1] == s.height) current = i;
        const auto apply = [self](std::uint32_t w, std::uint32_t h, bool full) {
            auto& settings = self->state.settings;
            std::string error;
            if (!self->platform_.apply_display(w, h, full, error)) {
                self->error_dialog(self->translate("Display settings could not be applied"), error);
                return;
            }
            settings.width = w; settings.height = h; settings.fullscreen = full;
            self->persist();
        };
        const auto step = [apply, current, &s](int delta) {
            const auto next = (current + std::size(resolutions) + delta) % std::size(resolutions);
            apply(resolutions[next][0], resolutions[next][1], s.fullscreen);
        };
        option(translate("Resolution"), std::to_string(s.width) + " x " + std::to_string(s.height), [step] { step(1); }, step);
        option(translate("Full screen"), on_off(s.fullscreen), [apply, &s] { apply(s.width, s.height, !s.fullscreen); },
               [apply, &s](int) { apply(s.width, s.height, !s.fullscreen); });
        break;
    }
    case 2: {
        const auto toggle = [self] { self->state.settings.ui_sounds = !self->state.settings.ui_sounds; self->persist(); };
        option(translate("Interface sounds"), on_off(s.ui_sounds), toggle, [toggle](int) { toggle(); });
        const auto volume = [self](int delta) {
            auto& v = self->state.settings.ui_volume;
            v = static_cast<std::uint32_t>(std::clamp(static_cast<int>(v) + delta * 10, 0, 100));
            self->persist();
            self->platform_.play(Sound::move);
        };
        option(translate("Interface volume"), std::to_string(s.ui_volume) + "%", [self, volume] {
            if (self->state.settings.ui_volume == 100) { self->state.settings.ui_volume = 0; self->persist(); } else volume(1);
        }, volume);
        break;
    }
    case 3: {
        const auto toggle = [self] {
            self->state.settings.vibration = !self->state.settings.vibration;
            self->persist();
            self->platform_.rumble();
        };
        option(translate("Vibration"), on_off(s.vibration), toggle, [toggle](int) { toggle(); });
        const auto zone = [self](int delta) {
            auto& z = self->state.settings.stick_deadzone;
            z = static_cast<std::uint32_t>(std::clamp(static_cast<int>(z) + delta * 5, 10, 80));
            self->persist();
        };
        option(translate("Stick dead zone"), std::to_string(s.stick_deadzone) + "%", [zone] { zone(1); }, zone);
        option(translate("Connected controllers"), std::to_string(platform_.controllers()), [self] { self->platform_.rumble(); ++self->serial; });
        break;
    }
    case 4: {
        for (const auto& language : {std::pair<std::string, std::string>{"en", "English"}, {"pt-BR", "Português (Brasil)"}}) {
            const auto code = language.first;
            option(language.second, s.language == code ? glyph(0xE73E) : std::string{}, [self, code] { self->state.settings.language = code; self->persist(); });
            out.back().selected = s.language == code;
        }
        break;
    }
    case 5: {
        option(translate("Console data"), from_path(root_), [self] { self->platform_.open_folder(self->root_); });
        if (const auto* p = profile()) {
            const auto folder = console::profile_storage(root_, *p);
            std::uint64_t bytes = 0;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(detail::file_path(folder), ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec)) bytes += it->file_size(ec);
            option(translate("Save data") + " - " + p->name, format_size(bytes), [self, folder] {
                std::error_code create;
                fs::create_directories(detail::file_path(folder), create);
                self->platform_.open_folder(folder);
            });
        }
        option(translate("Add library folder"), {}, [self] {
            std::filesystem::path folder;
            if (self->platform_.pick_folder(folder)) self->import_folder(folder);
        });
        option(translate("Rescan all"), std::to_string(s.library_folders.size()), [self] { self->rescan(); });
        for (const auto& folder : s.library_folders) {
            option(folder, translate("X: remove"), [self, folder] { self->import_folder(to_path(folder)); });
            out.back().secondary = [self, folder] {
                std::erase(self->state.settings.library_folders, folder);
                self->persist();
            };
        }
        break;
    }
    case 6: {
        const auto limit = [self](int delta) {
            constexpr std::uint32_t choices[] = {0, 5, 10};
            std::size_t index = 0;
            for (std::size_t i = 0; i < 3; ++i) if (choices[i] == self->state.settings.time_limit_minutes) index = i;
            self->state.settings.time_limit_minutes = choices[(index + 3 + delta) % 3];
            self->persist();
        };
        option(translate("Title time limit"), s.time_limit_minutes ? std::to_string(s.time_limit_minutes) + " " + translate("minutes") :
               translate("None (supervised)"), [limit] { limit(1); }, limit);
        option(translate("About"), "xone-native 0.1", [self] {
            Dialog d;
            d.title = "xone-native";
            d.lines = {self->translate("Locally recreated dashboard. Not Xbox One SystemOS."),
                       "Frame origin: local_recreation",
                       "ERA runtime: " + from_path(self->runtime_),
                       "Console data: " + from_path(self->root_),
                       "Not affiliated with or endorsed by Microsoft. No online services, licenses or achievements."};
            d.buttons = {self->translate("OK")};
            self->dialogs.push_back(std::move(d));
            ++self->serial;
        });
        option(translate("Exit xone-native"), {}, [self] {
            if (self->session_title.empty()) { self->platform_.request_exit(); return; }
            self->confirm(self->translate("Exit xone-native"), self->translate("Quitting closes the running title."),
                          self->translate("Exit"), [self] { self->platform_.request_exit(); });
        });
        break;
    }
    default: break;
    }
    return out;
}

std::vector<Item> Dashboard::guide_items() const {
    auto* self = const_cast<Dashboard*>(this);
    std::vector<Item> out;
    int y = 120;
    const auto entry = [&](std::string key, std::string label, char32_t icon, std::function<void()> run, int height = 64) {
        Item item;
        item.key = std::move(key);
        item.kind = Kind::menu;
        item.rect = {32, y, 496, height};
        item.label = std::move(label);
        item.glyph = icon ? glyph(icon) : std::string{};
        item.activate = [self, run = std::move(run)] { self->guide_open = false; run(); ++self->serial; };
        y += height + 8;
        out.push_back(std::move(item));
    };
    if (const auto* p = profile()) {
        entry("guide:profile", p->name, 0, [self] { self->history_.clear(); self->go(Page::sign_in); self->history_.clear(); }, 96);
        out.back().kind = Kind::profile;
        out.back().value = std::to_string(p->avatar);
        out.back().detail = translate("Switch profile");
    }
    entry("guide:home", translate("Home"), 0xE80F, [self] { self->history_.clear(); self->go(Page::home); self->history_.clear(); });
    entry("guide:library", translate("My games & apps"), 0xE7FC, [self] { self->go(Page::library); });
    if (const auto* t = title(session_title)) {
        y += 16;
        entry("guide:return", translate("Return to game") + ": " + t->name, 0xE768, [self] {
            self->go(Page::session);
            self->session_.focus_title();
        });
        entry("guide:quit", translate("Quit game") + ": " + t->name, 0xE711, [self] { self->quit_title(); });
        y += 16;
    }
    entry("guide:settings", translate("Settings"), 0xE713, [self] { self->go(Page::settings); });
    entry("guide:signout", translate("Sign out"), 0xE8AB, [self] {
        self->profile_id.clear();
        self->history_.clear();
        self->page = Page::sign_in;
    });
    entry("guide:exit", translate("Exit xone-native"), 0xE7E8, [self] {
        if (self->session_title.empty()) { self->platform_.request_exit(); return; }
        self->confirm(self->translate("Exit xone-native"), self->translate("Quitting closes the running title."),
                      self->translate("Exit"), [self] { self->platform_.request_exit(); });
    });
    return out;
}

void Dashboard::persist() {
    std::string error;
    if (!console::save(root_, state, error)) error_dialog(translate("The console state could not be saved"), error);
    ++serial;
}

void Dashboard::notify(const std::string& text, char32_t icon) {
    toasts.push_back({text, glyph(icon), 5.0});
    while (toasts.size() > 3) toasts.pop_front();
    ++serial;
}

void Dashboard::error_dialog(const std::string& heading, const std::string& summary, const std::vector<std::string>& tail) {
    Dialog d;
    d.title = heading;
    d.lines.push_back(summary);
    if (!tail.empty()) {
        d.lines.push_back({});
        d.lines.insert(d.lines.end(), tail.begin(), tail.end());
        d.monospace = true;
    }
    d.buttons = {translate("OK")};
    dialogs.push_back(std::move(d));
    platform_.play(Sound::error);
    ++serial;
}

void Dashboard::confirm(const std::string& heading, const std::string& line, const std::string& yes, std::function<void()> action) {
    Dialog d;
    d.title = heading;
    d.lines = {line};
    d.buttons = {yes, translate("Cancel")};
    d.focus = 1;
    d.choose = [action = std::move(action)](int index) { if (index == 0) action(); };
    dialogs.push_back(std::move(d));
    ++serial;
}

void Dashboard::open_keyboard(const std::string& purpose, const std::string& initial) {
    keyboard_purpose = purpose;
    keyboard_text = initial;
    keyboard_shift = initial.empty();
    focus_keys_[static_cast<int>(Page::keyboard)] = "key:1:0";
    go(Page::keyboard);
}

void Dashboard::finish_keyboard() {
    auto name = keyboard_text;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) { platform_.play(Sound::error); return; }
    if (keyboard_purpose == "rename") {
        if (auto* p = console::find_profile(state, profile_id)) p->name = name;
        persist();
        back();
        return;
    }
    console::Profile p;
    p.id = console::new_profile_id();
    p.name = name;
    p.avatar = static_cast<std::uint32_t>(state.profiles.size() % 12);
    p.created = platform_.now();
    state.profiles.push_back(p);
    persist();
    sign_in(p.id);
}

void Dashboard::sign_in(const std::string& id) {
    auto* p = console::find_profile(state, id);
    if (!p) return;
    p->last_signin = platform_.now();
    profile_id = id;
    state.settings.last_profile = id;
    persist();
    history_.clear();
    page = Page::home;
    scroll = 0;
    platform_.play(Sound::select);
    ++serial;
}

void Dashboard::go(Page next) {
    if (next == page) return;
    history_.push_back(page);
    page = next;
    scroll = 0;
    platform_.play(Sound::open);
    ++serial;
}

void Dashboard::back() {
    if (history_.empty()) return;
    page = history_.back();
    history_.pop_back();
    if (page == Page::session && session_title.empty()) return back();
    scroll = 0;
    platform_.play(Sound::back);
    ++serial;
}

void Dashboard::launch(const std::string& id) {
    auto* t = console::find_title(state, id);
    const auto* p = profile();
    if (!t || !p) return;
    if (id == session_title) {
        go(Page::session);
        session_.focus_title();
        return;
    }
    if (!session_title.empty()) {
        const auto running = title(session_title);
        confirm(translate("A title is running"), running ? running->name : session_title, translate("Quit game"), [this] { quit_title(); });
        return;
    }
    std::string error;
    if (!catalog::verify(*t, error)) {
        t->last_result = error;
        persist();
        error_dialog(translate("Could not start") + " " + t->name, error);
        return;
    }
    if (session_.active() && session_mode_ == session::Mode::check) {
        // A compatibility check yields to the user's launch; it is queued again.
        if (!checking_title.empty()) check_queue_.push_front(checking_title);
        pending_launch_ = id;
        session_.terminate();
        return;
    }
    const auto storage = console::title_storage(root_, *p, *t);
    session::Request request;
    request.mode = session::Mode::run;
    request.session_window = platform_.session_window();
    request.runtime = runtime_;
    request.title = to_path(t->executable);
    request.storage = storage;
    request.log = root_ / "logs" / "last-session.log";
    request.time_limit_ms = state.settings.time_limit_minutes * 60000u;
    if (!console::prepare_title_storage(storage, *p, error) || !session_.start(request, error)) {
        t->last_result = error;
        persist();
        error_dialog(translate("Could not start") + " " + t->name, error);
        return;
    }
    session_mode_ = session::Mode::run;
    session_title = id;
    t->last_played = platform_.now();
    ++t->play_count;
    persist();
    guide_open = false;
    go(Page::session);
    platform_.play(Sound::select);
    if (state.settings.vibration) platform_.rumble();
}

void Dashboard::check(const std::string& id) {
    if (!title(id)) return;
    std::erase(check_queue_, id);
    check_queue_.push_front(id);
    if (!session_.active()) next_check();
    else notify(translate("Checking") + "...", 0xE73E);
}

void Dashboard::next_check() {
    while (!check_queue_.empty() && !session_.active()) {
        const auto id = check_queue_.front();
        check_queue_.pop_front();
        auto* t = console::find_title(state, id);
        if (!t) continue;
        std::string error;
        if (!catalog::verify(*t, error)) {
            t->readiness = console::Readiness::blocked;
            t->check_detail = error;
            persist();
            continue;
        }
        session::Request request;
        request.mode = session::Mode::check;
        request.runtime = runtime_;
        request.title = to_path(t->executable);
        request.log = root_ / "logs" / "last-check.log";
        if (!session_.start(request, error)) {
            t->readiness = console::Readiness::blocked;
            t->check_detail = error;
            persist();
            check_queue_.clear(); // the runtime itself is unavailable; do not retry every title
            continue;
        }
        session_mode_ = session::Mode::check;
        checking_title = id;
        ++serial;
    }
}

void Dashboard::quit_title() {
    if (session_title.empty()) return;
    session_.terminate(); // Asks the title to suspend; a second quit ends it at once.
    const auto* t = title(session_title);
    notify(translate("Closing") + " " + (t ? t->name : session_title), 0xE711);
}

void Dashboard::finished(const session::Result& result) {
    if (session_mode_ == session::Mode::check) {
        const auto id = checking_title;
        checking_title.clear();
        if (auto* t = console::find_title(state, id); t && !result.terminated) {
            t->readiness = result.exit_code == 0 && result.mapped && result.unresolved == 0 ? console::Readiness::ready : console::Readiness::blocked;
            t->resolved = result.resolved;
            t->unresolved = result.unresolved;
            t->check_detail = result.summary;
            persist();
        }
        if (!pending_launch_.empty()) {
            const auto next = pending_launch_;
            pending_launch_.clear();
            launch(next);
        }
        ++serial;
        return;
    }
    const auto id = session_title;
    session_title.clear();
    auto* t = console::find_title(state, id);
    const auto name = t ? t->name : id;
    if (t) {
        t->last_result = result.summary + " (" + std::to_string(result.duration_ms / 1000) + " s)";
        persist();
    }
    platform_.focus_dashboard();
    if (page == Page::session) back();
    if (result.normal || result.terminated) notify(name + ": " + result.summary, 0xE73E);
    else error_dialog(name, result.summary, result.tail);
}

void Dashboard::tick(double seconds) {
    session::Result result;
    if (session_.poll(result)) finished(result);
    if (!session_.active() && !check_queue_.empty() && page != Page::session) next_check();
    for (auto& toast : toasts) toast.remaining -= seconds;
    while (!toasts.empty() && toasts.front().remaining <= 0) { toasts.pop_front(); ++serial; }
}

void Dashboard::text(char32_t c) {
    if (page != Page::keyboard || !dialogs.empty() || guide_open) return;
    if (c == U'\b') pop_utf8(keyboard_text);
    else if (c == U'\r') { finish_keyboard(); return; }
    else if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c <= 0xDFFF)) return;
    else if (utf8_length(keyboard_text) >= max_name_length) { platform_.play(Sound::error); return; }
    else {
        append_utf8(keyboard_text, c);
        if (keyboard_shift && c != U' ') keyboard_shift = false;
    }
    platform_.play(Sound::move);
    ++serial;
}

void Dashboard::move(Button b) {
    const auto list = guide_open ? guide_items() : items();
    const auto* current = focused(list);
    if (!current) return;
    if ((b == Button::left || b == Button::right) && current->adjust && !guide_open) {
        current->adjust(b == Button::right ? 1 : -1);
        ++serial;
        return;
    }
    const int cx = current->rect.x + current->rect.w / 2, cy = current->rect.y + current->rect.h / 2;
    const Item* best = nullptr;
    long best_score = 0;
    for (const auto& item : list) {
        if (&item == current) continue;
        const auto& r = item.rect;
        long primary = 0, secondary = 0;
        const int ix = r.x + r.w / 2, iy = r.y + r.h / 2;
        switch (b) {
            case Button::right: if (ix <= cx || r.x + r.w <= current->rect.x + current->rect.w) continue;
                primary = r.x - (current->rect.x + current->rect.w); break;
            case Button::left: if (ix >= cx || r.x >= current->rect.x) continue;
                primary = current->rect.x - (r.x + r.w); break;
            case Button::down: if (iy <= cy || r.y + r.h <= current->rect.y + current->rect.h) continue;
                primary = r.y - (current->rect.y + current->rect.h); break;
            case Button::up: if (iy >= cy || r.y >= current->rect.y) continue;
                primary = current->rect.y - (r.y + r.h); break;
            default: return;
        }
        const bool horizontal = b == Button::left || b == Button::right;
        if (horizontal) secondary = overlaps(r.y, r.y + r.h, current->rect.y, current->rect.y + current->rect.h) ? 0 : std::abs(iy - cy);
        else secondary = overlaps(r.x, r.x + r.w, current->rect.x, current->rect.x + current->rect.w) ? std::abs(ix - cx) / 4 : std::abs(ix - cx);
        const long score = std::max(primary, 0L) + secondary * 2;
        if (!best || score < best_score) { best = &item; best_score = score; }
    }
    if (!best) {
        if (b == Button::left && !guide_open && (page == Page::home || page == Page::library || page == Page::settings) && !profile_id.empty()) {
            guide_open = true;
            platform_.play(Sound::open);
            ++serial;
        }
        return;
    }
    focus_key() = best->key;
    if (page == Page::settings && !guide_open) {
        if (best->key.starts_with("cat:")) settings_category = std::stoi(best->key.substr(4));
        else if (current->key.starts_with("cat:")) focus_key() = "opt:0"; // entering a category starts at its first option
    }
    if (!guide_open) {
        const int top = best->rect.y - scroll, bottom = best->rect.y + best->rect.h - scroll;
        if (bottom > 1000) scroll += bottom - 1000;
        if (top < 150) scroll = std::max(0, scroll - (150 - top));
    }
    platform_.play(Sound::move);
    ++serial;
}

void Dashboard::input(Button b) {
    if (!dialogs.empty()) {
        auto& d = dialogs.back();
        const int count = static_cast<int>(d.buttons.size());
        if (b == Button::left || b == Button::up) d.focus = std::max(0, d.focus - 1);
        else if (b == Button::right || b == Button::down) d.focus = std::min(count - 1, d.focus + 1);
        else if (b == Button::a || b == Button::b) {
            const int choice = b == Button::a ? d.focus : count - 1;
            auto choose = std::move(d.choose);
            dialogs.pop_back();
            platform_.play(b == Button::a ? Sound::select : Sound::back);
            if (choose) choose(choice);
        } else return;
        if (b != Button::a && b != Button::b) platform_.play(Sound::move);
        ++serial;
        return;
    }
    const bool signed_in = !profile_id.empty();
    if (b == Button::guide) {
        if (!signed_in) return;
        guide_open = !guide_open;
        if (guide_open) { guide_focus.clear(); platform_.focus_dashboard(); }
        else if (page == Page::session && !session_title.empty()) session_.focus_title();
        platform_.play(guide_open ? Sound::open : Sound::back);
        ++serial;
        return;
    }
    if (guide_open && b == Button::b) {
        guide_open = false;
        if (page == Page::session && !session_title.empty()) session_.focus_title();
        platform_.play(Sound::back);
        ++serial;
        return;
    }
    switch (b) {
        case Button::up: case Button::down: case Button::left: case Button::right: move(b); return;
        default: break;
    }
    const auto list = guide_open ? guide_items() : items();
    const auto* current = focused(list);
    if (b == Button::a) {
        if (current && current->activate) {
            if (current->kind != Kind::key) platform_.play(Sound::select);
            current->activate();
            ++serial;
        }
        return;
    }
    if (guide_open) return;
    if (page == Page::keyboard) {
        if (b == Button::x) text(U'\b');
        else if (b == Button::y) text(U' ');
        else if (b == Button::menu) finish_keyboard();
        else if (b == Button::view) { keyboard_shift = !keyboard_shift; ++serial; }
        else if (b == Button::b) back();
        return;
    }
    switch (b) {
        case Button::b:
            if (page == Page::settings && current && current->key.starts_with("opt:")) {
                focus_key() = "cat:" + std::to_string(settings_category);
                platform_.play(Sound::back);
                ++serial;
            } else back();
            break;
        case Button::menu: if (current && current->manage) current->manage(); break;
        case Button::x: if (current && current->secondary) { current->secondary(); platform_.play(Sound::select); } break;
        case Button::lb: case Button::rb: {
            if (!signed_in || page == Page::session) break;
            constexpr Page tabs[] = {Page::home, Page::library, Page::settings};
            int index = 0;
            for (int i = 0; i < 3; ++i) if (tabs[i] == page) index = i;
            index = (index + (b == Button::rb ? 1 : 2)) % 3;
            history_.clear();
            if (tabs[index] != Page::home) history_.push_back(Page::home);
            page = tabs[index];
            scroll = 0;
            platform_.play(Sound::move);
            ++serial;
            break;
        }
        default: break;
    }
}

void Dashboard::import_folder(const fs::path& folder) {
    const auto text = from_path(fs::absolute(folder));
    auto& folders = state.settings.library_folders;
    const auto result = catalog::scan(folder);
    const auto before = state.titles.size();
    catalog::merge(state, result, platform_.now());
    for (std::size_t i = before; i < state.titles.size(); ++i) check_queue_.push_back(state.titles[i].id);
    if (std::find(folders.begin(), folders.end(), text) == folders.end() && !result.titles.empty()) folders.push_back(text);
    persist();
    if (result.titles.empty()) {
        std::vector<std::string> lines(result.errors.begin(), result.errors.begin() + std::min<std::size_t>(result.errors.size(), 12));
        error_dialog(translate("No titles found") + ": " + text, result.errors.empty() ? "AppxManifest.xml" : result.errors.front(), lines);
        return;
    }
    notify(translate("Imported titles") + ": " + std::to_string(state.titles.size() - before) + " / " + std::to_string(result.titles.size()), 0xE8B7);
}

void Dashboard::rescan() {
    std::size_t found = 0, added = 0;
    std::vector<std::string> errors;
    for (const auto& folder : state.settings.library_folders) {
        const auto result = catalog::scan(to_path(folder));
        const auto before = state.titles.size();
        catalog::merge(state, result, platform_.now());
        for (std::size_t i = before; i < state.titles.size(); ++i) check_queue_.push_back(state.titles[i].id);
        found += result.titles.size();
        added += state.titles.size() - before;
        errors.insert(errors.end(), result.errors.begin(), result.errors.end());
    }
    persist();
    notify(translate("Rescan library") + ": " + std::to_string(found) + " (+" + std::to_string(added) + ")", 0xE72C);
    if (!errors.empty()) error_dialog(translate("Rescan library"), errors.front(),
                                      std::vector<std::string>(errors.begin(), errors.begin() + std::min<std::size_t>(errors.size(), 12)));
}

} // namespace xboxone::dashboard

#ifndef _WIN32
#include <cstdio>
namespace xboxone::dashboard {
int run_host() {
    std::fprintf(stderr, "xone-native: the dashboard host adapter is implemented for Windows only on this revision\n");
    return 2;
}
} // namespace xboxone::dashboard
#endif
