#include "normal.hh"

#include "buffer.hh"
#include "buffer_manager.hh"
#include "client_manager.hh"
#include "command_manager.hh"
#include "commands.hh"
#include "containers.hh"
#include "context.hh"
#include "debug.hh"
#include "face_registry.hh"
#include "flags.hh"
#include "file.hh"
#include "option_manager.hh"
#include "register_manager.hh"
#include "selectors.hh"
#include "shell_manager.hh"
#include "string.hh"
#include "user_interface.hh"
#include "window.hh"

namespace Kakoune
{

using namespace std::placeholders;

enum class SelectMode
{
    Replace,
    Extend,
    Append,
};

template<SelectMode mode = SelectMode::Replace, typename Func>
void select(Context& context, Func func)
{
    auto& buffer = context.buffer();
    auto& selections = context.selections();
    if (mode == SelectMode::Append)
    {
        auto& sel = selections.main();
        auto  res = func(buffer, sel);
        if (res.captures().empty())
            res.captures() = sel.captures();
        selections.push_back(res);
        selections.set_main_index(selections.size() - 1);
    }
    else
    {
        for (auto& sel : selections)
        {
            auto res = func(buffer, sel);
            if (mode == SelectMode::Extend)
                sel.merge_with(res);
            else
            {
                sel.anchor() = res.anchor();
                sel.cursor() = res.cursor();
            }
            if (not res.captures().empty())
                sel.captures() = std::move(res.captures());
        }
    }
    selections.sort_and_merge_overlapping();
    selections.check_invariant();
}

template<SelectMode mode, typename T>
class Select
{
public:
    constexpr Select(T t) : m_func(t) {}
    void operator() (Context& context, NormalParams) { select<mode>(context, m_func); }
private:
    T m_func;
};

template<SelectMode mode = SelectMode::Replace, typename T>
constexpr Select<mode, T> make_select(T func)
{
    return Select<mode, T>(func);
}

template<SelectMode mode = SelectMode::Replace>
void select_coord(Buffer& buffer, ByteCoord coord, SelectionList& selections)
{
    coord = buffer.clamp(coord);
    if (mode == SelectMode::Replace)
        selections = SelectionList{ buffer, coord };
    else if (mode == SelectMode::Extend)
    {
        for (auto& sel : selections)
            sel.cursor() = coord;
        selections.sort_and_merge_overlapping();
    }
}

template<InsertMode mode>
void enter_insert_mode(Context& context, NormalParams)
{
    context.input_handler().insert(mode);
}

void repeat_last_insert(Context& context, NormalParams)
{
    context.input_handler().repeat_last_insert();
}

bool show_auto_info_ifn(StringView title, StringView info,
                        const Context& context)
{
    if (context.options()["autoinfo"].get<int>() < 1 or not context.has_ui())
        return false;
    Face face = get_face("Information");
    context.ui().info_show(title, info, CharCoord{}, face, InfoStyle::Prompt);
    return true;
}

template<typename Cmd>
void on_next_key_with_autoinfo(const Context& context, KeymapMode keymap_mode, Cmd cmd,
                               StringView title, StringView info)
{
    const bool hide = show_auto_info_ifn(title, info, context);
    context.input_handler().on_next_key(
        keymap_mode, [hide,cmd](Key key, Context& context) mutable {
            if (hide)
                context.ui().info_hide();
            cmd(key, context);
    });
}

template<SelectMode mode>
void goto_commands(Context& context, NormalParams params)
{
    if (params.count != 0)
    {
        context.push_jump();
        select_coord<mode>(context.buffer(), LineCount{params.count - 1}, context.selections());
        if (context.has_window())
            context.window().center_line(LineCount{params.count-1});
    }
    else
    {
        on_next_key_with_autoinfo(context, KeymapMode::Goto,
                                 [](Key key, Context& context) {
            if (key.modifiers != Key::Modifiers::None)
                return;
            auto& buffer = context.buffer();
            switch (tolower(key.key))
            {
            case 'g':
            case 'k':
                context.push_jump();
                select_coord<mode>(buffer, ByteCoord{0,0}, context.selections());
                break;
            case 'l':
                select<mode>(context, select_to_eol);
                break;
            case 'h':
                select<mode>(context, select_to_eol_reverse);
                break;
            case 'j':
            {
                context.push_jump();
                select_coord<mode>(buffer, buffer.line_count() - 1, context.selections());
                break;
            }
            case 'e':
                context.push_jump();
                select_coord<mode>(buffer, buffer.back_coord(), context.selections());
                break;
            case 't':
                if (context.has_window())
                {
                    auto line = context.window().position().line;
                    select_coord<mode>(buffer, line, context.selections());
                }
                break;
            case 'b':
                if (context.has_window())
                {
                    auto& window = context.window();
                    auto line = window.position().line + window.dimensions().line - 1;
                    select_coord<mode>(buffer, line, context.selections());
                }
                break;
            case 'c':
                if (context.has_window())
                {
                    auto& window = context.window();
                    auto line = window.position().line + window.dimensions().line / 2;
                    select_coord<mode>(buffer, line, context.selections());
                }
                break;
            case 'a':
            {
                auto& buffer_manager = BufferManager::instance();
                auto it = buffer_manager.begin();
                if (it->get() == &buffer and ++it == buffer_manager.end())
                    break;
                Buffer& target = **it;
                BufferManager::instance().set_last_used_buffer(buffer);
                context.push_jump();
                context.change_buffer(target);
                break;
            }
            case 'f':
            {
                const Selection& sel = context.selections().main();
                String filename = content(buffer, sel);
                static constexpr char forbidden[] = { '\'', '\\', '\0' };
                for (auto c : forbidden)
                    if (contains(filename, c))
                        return;

                auto paths = context.options()["path"].get<Vector<String, MemoryDomain::Options>>();
                const String& buffer_name = buffer.name();
                auto it = find(reversed(buffer_name), '/');
                if (it != buffer_name.rend())
                    paths.insert(paths.begin(), String{buffer_name.begin(), it.base()});

                String path = find_file(filename, paths);
                if (path.empty())
                    throw runtime_error("unable to find file '" + filename + "'");

                Buffer* buffer = create_buffer_from_file(path);
                if (buffer == nullptr)
                    throw runtime_error("unable to open file '" + path + "'");

                if (buffer != &context.buffer())
                {
                    BufferManager::instance().set_last_used_buffer(*buffer);
                    context.push_jump();
                    context.change_buffer(*buffer);
                }
                break;
            }
            case '.':
            {
                context.push_jump();
                auto pos = buffer.last_modification_coord();
                if (buffer[pos.line].length() == pos.column + 1)
                    pos = ByteCoord{ pos.line+1, 0 };
                select_coord<mode>(buffer, pos, context.selections());
                break;
            }
            }
        }, "goto",
        "g,k:  buffer top        \n"
        "l:    line end          \n"
        "h:    line begin        \n"
        "j:    buffer bottom     \n"
        "e:    buffer end        \n"
        "t:    window top        \n"
        "b:    window bottom     \n"
        "c:    window center     \n"
        "a:    last buffer       \n"
        "f:    file              \n"
        ".:    last buffer change\n");
    }
}

void view_commands(Context& context, NormalParams params)
{
    on_next_key_with_autoinfo(context, KeymapMode::View,
                             [params](Key key, Context& context) {
        if (key.modifiers != Key::Modifiers::None or not context.has_window())
            return;

        LineCount cursor_line = context.selections().main().cursor().line;
        Window& window = context.window();
        switch (tolower(key.key))
        {
        case 'v':
        case 'c':
            context.window().center_line(cursor_line);
            break;
        case 't':
            context.window().display_line_at(cursor_line, 0);
            break;
        case 'b':
            context.window().display_line_at(cursor_line, window.dimensions().line-1);
            break;
        case 'h':
            context.window().scroll(-std::max<CharCount>(1, params.count));
            break;
        case 'j':
            context.window().scroll( std::max<LineCount>(1, params.count));
            break;
        case 'k':
            context.window().scroll(-std::max<LineCount>(1, params.count));
            break;
        case 'l':
            context.window().scroll( std::max<CharCount>(1, params.count));
            break;
        }
    }, "view",
    "v,c:  center cursor   \n"
    "t:    cursor on top   \n"
    "b:    cursor on bottom\n"
    "h:    scroll left     \n"
    "j:    scroll down     \n"
    "k:    scroll up       \n"
    "l:    scroll right    \n");
}

void replace_with_char(Context& context, NormalParams)
{
    on_next_key_with_autoinfo(context, KeymapMode::None,
                             [](Key key, Context& context) {
        if (not iswprint(key.key))
            return;
        ScopedEdition edition(context);
        Buffer& buffer = context.buffer();
        SelectionList& selections = context.selections();
        Vector<String> strings;
        for (auto& sel : selections)
        {
            CharCount count = char_length(buffer, sel);
            strings.emplace_back(key.key, count);
        }
        selections.insert(strings, InsertMode::Replace);
    }, "replace with char", "enter char to replace with\n");
}

Codepoint to_lower(Codepoint cp) { return tolower(cp); }
Codepoint to_upper(Codepoint cp) { return toupper(cp); }

Codepoint swap_case(Codepoint cp)
{
    Codepoint res = std::tolower(cp);
    return res == cp ? std::toupper(cp) : res;
}

template<Codepoint (*func)(Codepoint)>
void for_each_char(Context& context, NormalParams)
{
    ScopedEdition edition(context);
    Vector<String> sels = context.selections_content();
    for (auto& sel : sels)
    {
        for (auto& c : sel)
            c = func(c);
    }
    context.selections().insert(sels, InsertMode::Replace);
}

void command(Context& context, NormalParams)
{
    if (not CommandManager::has_instance())
        return;

    context.input_handler().prompt(
        ":", "", get_face("Prompt"),
        std::bind(&CommandManager::complete, &CommandManager::instance(), _1, _2, _3, _4),
        [](StringView cmdline, PromptEvent event, Context& context) {
            if (context.has_ui())
            {
                context.ui().info_hide();
                if (event == PromptEvent::Change and context.options()["autoinfo"].get<int>() > 0)
                {
                    auto info = CommandManager::instance().command_info(context, cmdline);
                    Face col = get_face("Information");
                    if (not info.first.empty() and not info.second.empty())
                        context.ui().info_show(info.first, info.second, CharCoord{}, col, InfoStyle::Prompt);
                }
            }
            if (event == PromptEvent::Validate)
                CommandManager::instance().execute(cmdline, context);
        });
}

template<bool replace>
void pipe(Context& context, NormalParams)
{
    const char* prompt = replace ? "pipe:" : "pipe-to:";
    context.input_handler().prompt(prompt, "", get_face("Prompt"), shell_complete,
        [](StringView cmdline, PromptEvent event, Context& context)
        {
            if (event != PromptEvent::Validate)
                return;

            StringView real_cmd;
            if (cmdline.empty())
                real_cmd = context.main_sel_register_value("|");
            else
            {
                RegisterManager::instance()['|'] = String{cmdline};
                real_cmd = cmdline;
            }

            if (real_cmd.empty())
                return;

            Buffer& buffer = context.buffer();
            SelectionList& selections = context.selections();
            if (replace)
            {
                Vector<String> strings;
                for (auto& sel : selections)
                {
                    auto str = content(buffer, sel);
                    bool insert_eol = str.back() != '\n';
                    if (insert_eol)
                        str += '\n';
                    str = ShellManager::instance().pipe(str, real_cmd, context,
                                                        {}, EnvVarMap{});
                    if (insert_eol and str.back() == '\n')
                        str = str.substr(0, str.length()-1);
                    strings.push_back(std::move(str));
                }
                ScopedEdition edition(context);
                selections.insert(strings, InsertMode::Replace);
            }
            else
            {
                for (auto& sel : selections)
                    ShellManager::instance().pipe(
                        content(buffer, sel), real_cmd, context, {},
                        EnvVarMap{});
            }
        });
}

template<InsertMode mode>
void insert_output(Context& context, NormalParams)
{
    const char* prompt = mode == InsertMode::Insert ? "insert-output:" : "append-output:";
    context.input_handler().prompt(prompt, "", get_face("Prompt"), shell_complete,
        [](StringView cmdline, PromptEvent event, Context& context)
        {
            if (event != PromptEvent::Validate)
                return;

            StringView real_cmd;
            if (cmdline.empty())
                real_cmd = context.main_sel_register_value("|");
            else
            {
                RegisterManager::instance()['|'] = String{cmdline};
                real_cmd = cmdline;
            }

            if (real_cmd.empty())
                return;

            auto str = ShellManager::instance().eval(real_cmd, context, {},
                                                     EnvVarMap{});
            ScopedEdition edition(context);
            context.selections().insert(str, mode);
        });
}

template<Direction direction, SelectMode mode>
void select_next_match(const Buffer& buffer, SelectionList& selections,
                       const Regex& regex)
{
    if (mode == SelectMode::Replace)
    {
        for (auto& sel : selections)
            sel = keep_direction(find_next_match<direction>(buffer, sel, regex), sel);
    }
    if (mode == SelectMode::Extend)
    {
        for (auto& sel : selections)
            sel.merge_with(find_next_match<direction>(buffer, sel, regex));
    }
    else if (mode == SelectMode::Append)
    {
        auto sel = keep_direction(
            find_next_match<direction>(buffer, selections.main(), regex),
            selections.main());
        selections.push_back(std::move(sel));
        selections.set_main_index(selections.size() - 1);
    }
    selections.sort_and_merge_overlapping();
}

void yank(Context& context, NormalParams params)
{
    RegisterManager::instance()[params.reg] = context.selections_content();
    context.print_status({ "yanked " + to_string(context.selections().size()) +
                           " selections to register " + StringView{params.reg},
                           get_face("Information") });
}

void erase_selections(Context& context, NormalParams params)
{
    RegisterManager::instance()[params.reg] = context.selections_content();
    ScopedEdition edition(context);
    context.selections().erase();
    context.selections().avoid_eol();
}

void change(Context& context, NormalParams params)
{
    RegisterManager::instance()[params.reg] = context.selections_content();
    enter_insert_mode<InsertMode::Replace>(context, params);
}

constexpr InsertMode adapt_for_linewise(InsertMode mode)
{
    return ((mode == InsertMode::Append) ?
             InsertMode::InsertAtNextLineBegin :
             ((mode == InsertMode::Insert) ?
               InsertMode::InsertAtLineBegin :
               ((mode == InsertMode::Replace) ?
                 InsertMode::Replace : InsertMode::Insert)));
}

template<InsertMode mode>
void paste(Context& context, NormalParams params)
{
    auto strings = RegisterManager::instance()[params.reg].values(context);
    InsertMode effective_mode = mode;
    for (auto& str : strings)
    {
        if (not str.empty() and str.back() == '\n')
        {
            effective_mode = adapt_for_linewise(mode);
            break;
        }
    }
    ScopedEdition edition(context);
    context.selections().insert(strings, effective_mode);
}

template<InsertMode mode>
void paste_all(Context& context, NormalParams params)
{
    auto strings = RegisterManager::instance()[params.reg].values(context);
    InsertMode effective_mode = mode;
    String all;
    Vector<ByteCount> offsets;
    for (auto& str : strings)
    {
        if (not str.empty() and str.back() == '\n')
            effective_mode = adapt_for_linewise(mode);
        all += str;
        offsets.push_back(all.length());
    }

    auto& selections = context.selections();
    {
        ScopedEdition edition(context);
        selections.insert(all, effective_mode, true);
    }

    const Buffer& buffer = context.buffer();
    Vector<Selection> result;
    for (auto& selection : selections)
    {
        ByteCount pos = 0;
        for (auto offset : offsets)
        {
            result.push_back({ buffer.advance(selection.min(), pos),
                               buffer.advance(selection.min(), offset-1) });
            pos = offset;
        }
    }
    selections = std::move(result);
}

template<typename T>
void regex_prompt(Context& context, const String prompt, T func)
{
    SelectionList selections = context.selections();
    context.input_handler().prompt(prompt, "", get_face("Prompt"), complete_nothing,
        [=](StringView str, PromptEvent event, Context& context) mutable {
            try
            {
                if (event != PromptEvent::Change and context.has_ui())
                    context.ui().info_hide();
                selections.update();
                context.selections() = selections;
                context.input_handler().set_prompt_face(get_face("Prompt"));
                if (event == PromptEvent::Abort)
                    return;
                if (event == PromptEvent::Change and
                    (str.empty() or not context.options()["incsearch"].get<bool>()))
                    return;

                if (event == PromptEvent::Validate)
                    context.push_jump();
                Regex regex = str.empty() ? Regex{}
                                          : Regex{str.begin(), str.end()};
                func(std::move(regex), event, context);
            }
            catch (RegexError& err)
            {
                if (event == PromptEvent::Validate)
                    throw runtime_error("regex error: "_str + err.what());
                else
                    context.input_handler().set_prompt_face(get_face("Error"));
            }
            catch (std::runtime_error& err)
            {
                if (event == PromptEvent::Validate)
                    throw runtime_error("regex error: "_str + err.what());
                else
                {
                    context.input_handler().set_prompt_face(get_face("Error"));
                    if (context.has_ui())
                    {
                        Face face = get_face("Information");
                        context.ui().info_show("regex error", err.what(), CharCoord{}, face, InfoStyle::Prompt);
                    }
                }
            }
            catch (runtime_error&)
            {
                context.selections() = selections;
                // only validation should propagate errors,
                // incremental search should not.
                if (event == PromptEvent::Validate)
                    throw;
            }
        });
}

template<SelectMode mode, Direction direction>
void search(Context& context, NormalParams)
{
    regex_prompt(context, direction == Forward ? "search:" : "reverse search:",
                 [](Regex ex, PromptEvent event, Context& context) {
                     if (ex.empty())
                         ex = Regex{context.main_sel_register_value("/").str()};
                     else if (event == PromptEvent::Validate)
                         RegisterManager::instance()['/'] = String{ex.str()};
                     if (not ex.empty() and not ex.str().empty())
                         select_next_match<direction, mode>(context.buffer(), context.selections(), ex);
                 });
}

template<SelectMode mode, Direction direction>
void search_next(Context& context, NormalParams params)
{
    StringView str = context.main_sel_register_value("/");
    if (not str.empty())
    {
        try
        {
            Regex ex{str.begin(), str.end()};
            do {
                select_next_match<direction, mode>(context.buffer(), context.selections(), ex);
            } while (--params.count > 0);
        }
        catch (RegexError& err)
        {
            throw runtime_error("regex error: "_str + err.what());
        }
    }
    else
        throw runtime_error("no search pattern");
}

template<bool smart>
void use_selection_as_search_pattern(Context& context, NormalParams)
{
    Vector<String> patterns;
    auto& sels = context.selections();
    const auto& buffer = context.buffer();
    for (auto& sel : sels)
    {
        auto begin = utf8::make_iterator(buffer.iterator_at(sel.min()));
        auto end   = utf8::make_iterator(buffer.iterator_at(sel.max()))+1;
        auto content = "\\Q" + String{begin.base(), end.base()} + "\\E";
        if (smart)
        {
            if (begin == buffer.begin() or (is_word(*begin) and not is_word(*(begin-1))))
                content = "\\b" + content;
            if (end == buffer.end() or (is_word(*(end-1)) and not is_word(*end)))
                content = content + "\\b";
        }
        patterns.push_back(std::move(content));
    }
    RegisterManager::instance()['/'] = patterns;
}

void select_regex(Context& context, NormalParams)
{
    regex_prompt(context, "select:", [](Regex ex, PromptEvent event, Context& context) {
        if (ex.empty())
            ex = Regex{context.main_sel_register_value("/").str()};
        else if (event == PromptEvent::Validate)
            RegisterManager::instance()['/'] = String{ex.str()};
        if (not ex.empty() and not ex.str().empty())
            select_all_matches(context.selections(), ex);
    });
}

void split_regex(Context& context, NormalParams)
{
    regex_prompt(context, "split:", [](Regex ex, PromptEvent event, Context& context) {
        if (ex.empty())
            ex = Regex{context.main_sel_register_value("/").str()};
        else if (event == PromptEvent::Validate)
            RegisterManager::instance()['/'] = String{ex.str()};
        if (not ex.empty() and not ex.str().empty())
            split_selections(context.selections(), ex);
    });
}

void split_lines(Context& context, NormalParams)
{
    auto& selections = context.selections();
    auto& buffer = context.buffer();
    Vector<Selection> res;
    for (auto& sel : selections)
    {
        if (sel.anchor().line == sel.cursor().line)
        {
             res.push_back(std::move(sel));
             continue;
        }
        auto min = sel.min();
        auto max = sel.max();
        res.push_back(keep_direction({min, {min.line, buffer[min.line].length()-1}}, sel));
        for (auto line = min.line+1; line < max.line; ++line)
            res.push_back(keep_direction({line, {line, buffer[line].length()-1}}, sel));
        res.push_back(keep_direction({max.line, max}, sel));
    }
    selections = std::move(res);
}

void join_lines_select_spaces(Context& context, NormalParams)
{
    auto& buffer = context.buffer();
    Vector<Selection> selections;
    for (auto& sel : context.selections())
    {
        const LineCount min_line = sel.min().line;
        const LineCount max_line = sel.max().line;
        auto end_line = std::min(buffer.line_count()-1,
                                 max_line + (min_line == max_line ? 1 : 0));
        for (LineCount line = min_line; line < end_line; ++line)
        {
            auto begin = buffer.iterator_at({line, buffer[line].length()-1});
            auto end = begin+1;
            skip_while(end, buffer.end(), is_horizontal_blank);
            selections.push_back({begin.coord(), (end-1).coord()});
        }
    }
    if (selections.empty())
        return;
    context.selections() = selections;
    ScopedEdition edition(context);
    context.selections().insert(" "_str, InsertMode::Replace);
}

void join_lines(Context& context, NormalParams params)
{
    SelectionList sels{context.selections()};
    auto restore_sels = on_scope_end([&]{
        sels.update();
        context.selections() = std::move(sels);
    });

    join_lines_select_spaces(context, params);
}

template<bool matching>
void keep(Context& context, NormalParams)
{
    constexpr const char* prompt = matching ? "keep matching:" : "keep not matching:";
    regex_prompt(context, prompt, [](const Regex& ex, PromptEvent, Context& context) {
        if (ex.empty())
            return;
        const Buffer& buffer = context.buffer();
        Vector<Selection> keep;
        for (auto& sel : context.selections())
        {
            if (regex_search(buffer.iterator_at(sel.min()),
                             utf8::next(buffer.iterator_at(sel.max()), buffer.end()), ex) == matching)
                keep.push_back(sel);
        }
        if (keep.empty())
            throw runtime_error("no selections remaining");
        context.set_selections(std::move(keep));
    });
}

void keep_pipe(Context& context, NormalParams)
{
    context.input_handler().prompt(
        "keep pipe:", "", get_face("Prompt"), shell_complete,
        [](StringView cmdline, PromptEvent event, Context& context) {
            if (event != PromptEvent::Validate)
                return;
            const Buffer& buffer = context.buffer();
            auto& shell_manager = ShellManager::instance();
            Vector<Selection> keep;
            for (auto& sel : context.selections())
            {
                int status = 0;
                shell_manager.pipe(content(buffer, sel), cmdline, context,
                                   {}, EnvVarMap{}, &status);
                if (status == 0)
                    keep.push_back(sel);
            }
            if (keep.empty())
                throw runtime_error("no selections remaining");
            context.set_selections(std::move(keep));
    });
}
template<bool indent_empty = false>
void indent(Context& context, NormalParams)
{
    CharCount indent_width = context.options()["indentwidth"].get<int>();
    String indent = indent_width == 0 ? "\t" : String{' ', indent_width};

    auto& buffer = context.buffer();
    Vector<Selection> sels;
    LineCount last_line = 0;
    for (auto& sel : context.selections())
    {
        for (auto line = std::max(last_line, sel.min().line); line < sel.max().line+1; ++line)
        {
            if (indent_empty or buffer[line].length() > 1)
                sels.push_back({line, line});
        }
        // avoid reindenting the same line if multiple selections are on it
        last_line = sel.max().line+1;
    }
    if (not sels.empty())
    {
        ScopedEdition edition(context);
        SelectionList selections{buffer, std::move(sels)};
        selections.insert(indent, InsertMode::Insert);
    }
}

template<bool deindent_incomplete = true>
void deindent(Context& context, NormalParams)
{
    CharCount tabstop = context.options()["tabstop"].get<int>();
    CharCount indent_width = context.options()["indentwidth"].get<int>();
    if (indent_width == 0)
        indent_width = tabstop;

    auto& buffer = context.buffer();
    Vector<Selection> sels;
    LineCount last_line = 0;
    for (auto& sel : context.selections())
    {
        for (auto line = std::max(sel.min().line, last_line);
             line < sel.max().line+1; ++line)
        {
            CharCount width = 0;
            auto content = buffer[line];
            for (auto column = 0_byte; column < content.length(); ++column)
            {
                const char c = content[column];
                if (c == '\t')
                    width = (width / tabstop + 1) * tabstop;
                else if (c == ' ')
                    ++width;
                else
                {
                    if (deindent_incomplete and width != 0)
                        sels.push_back({ line, ByteCoord{line, column-1} });
                    break;
                }
                if (width == indent_width)
                {
                    sels.push_back({ line, ByteCoord{line, column} });
                    break;
                }
            }
        }
        // avoid reindenting the same line if multiple selections are on it
        last_line = sel.max().line + 1;
    }
    if (not sels.empty())
    {
        ScopedEdition edition(context);
        SelectionList selections{context.buffer(), std::move(sels)};
        selections.erase();
    }
}

template<ObjectFlags flags, SelectMode mode = SelectMode::Replace>
void select_object(Context& context, NormalParams params)
{
    int level = params.count <= 0 ? 0 : params.count - 1;
    on_next_key_with_autoinfo(context, KeymapMode::None,
                             [level](Key key, Context& context) {
        if (key.modifiers != Key::Modifiers::None)
            return;
        const Codepoint c = key.key;

        static constexpr struct
        {
            Codepoint key;
            Selection (*func)(const Buffer&, const Selection&, ObjectFlags);
        } selectors[] = {
            { 'w', select_word<Word> },
            { 'W', select_word<WORD> },
            { 's', select_sentence },
            { 'p', select_paragraph },
            { ' ', select_whitespaces },
            { 'i', select_indent },
            { 'n', select_number },
        };
        for (auto& sel : selectors)
        {
            if (c == sel.key)
                return select<mode>(context, std::bind(sel.func, _1, _2, flags));
        }

        static const struct
        {
            CodepointPair pair;
            Codepoint name;
        } surrounding_pairs[] = {
            { { '(', ')' }, 'b' },
            { { '{', '}' }, 'B' },
            { { '[', ']' }, 'r' },
            { { '<', '>' }, 'a' },
            { { '"', '"' }, 'Q' },
            { { '\'', '\'' }, 'q' },
            { { '`', '`' }, 'g' },
        };
        for (auto& sur : surrounding_pairs)
        {
            if (sur.pair.first == c or sur.pair.second == c or
                (sur.name != 0 and sur.name == c))
                return select<mode>(context, std::bind(select_surrounding, _1, _2,
                                                       sur.pair, level, flags));
        }
    }, "select object",
    "b,(,):  parenthesis block\n"
    "B,{,}:  braces block     \n"
    "r,[,]:  brackets block   \n"
    "a,<,>:  angle block      \n"
    "\",Q:  double quote string\n"
    "',q:  single quote string\n"
    "`,g:  grave quote string \n"
    "w:    word               \n"
    "W:    WORD               \n"
    "s:    sentence           \n"
    "p:    paragraph          \n"
    "␣:    whitespaces        \n"
    "i:    indent             \n");
}

template<Key::NamedKey key>
void scroll(Context& context, NormalParams)
{
    static_assert(key == Key::PageUp or key == Key::PageDown,
                  "scrool only implements PageUp and PageDown");
    Window& window = context.window();
    Buffer& buffer = context.buffer();
    CharCoord position = window.position();
    LineCount cursor_line = 0;

    if (key == Key::PageUp)
    {
        position.line -= (window.dimensions().line - 2);
        cursor_line = position.line;
    }
    else if (key == Key::PageDown)
    {
        position.line += (window.dimensions().line - 2);
        cursor_line = position.line + window.dimensions().line - 1;
    }
    auto cursor_pos = utf8::advance(buffer.iterator_at(position.line),
                                    buffer.iterator_at(position.line+1),
                                    position.column);
    select_coord(buffer, cursor_pos.coord(), context.selections());
    window.set_position(position);
}

template<Direction direction>
void copy_selections_on_next_lines(Context& context, NormalParams params)
{
    auto& selections = context.selections();
    auto& buffer = context.buffer();
    Vector<Selection> result;
    for (auto& sel : selections)
    {
        auto anchor = sel.anchor();
        auto cursor = sel.cursor();
        result.push_back(std::move(sel));
        for (int i = 0; i < std::max(params.count, 1); ++i)
        {
            LineCount offset = (direction == Forward ? 1 : -1) * (i + 1);
            ByteCoord new_anchor{anchor.line + offset, anchor.column};
            ByteCoordAndTarget new_cursor{cursor.line + offset, cursor.column, cursor.target};
            if (buffer.is_valid(new_anchor) and buffer.is_valid(new_cursor))
                result.emplace_back(new_anchor, new_cursor);
        }
    }
    selections = std::move(result);
    selections.sort_and_merge_overlapping();
}

void rotate_selections(Context& context, NormalParams params)
{
    context.selections().rotate_main(params.count != 0 ? params.count : 1);
}

void rotate_selections_content(Context& context, NormalParams params)
{
    int group = params.count;
    int count = 1;
    auto strings = context.selections_content();
    if (group == 0 or group > (int)strings.size())
        group = (int)strings.size();
    count = count % group;
    for (auto it = strings.begin(); it != strings.end(); )
    {
        auto end = std::min(strings.end(), it + group);
        std::rotate(it, end-count, end);
        it = end;
    }
    context.selections().insert(strings, InsertMode::Replace);
    context.selections().rotate_main(count);
}

enum class SelectFlags
{
    None = 0,
    Reverse = 1,
    Inclusive = 2,
    Extend = 4
};

template<> struct WithBitOps<SelectFlags> : std::true_type {};

template<SelectFlags flags>
void select_to_next_char(Context& context, NormalParams params)
{
    on_next_key_with_autoinfo(context, KeymapMode::None,
                             [params](Key key, Context& context) {
        select<flags & SelectFlags::Extend ? SelectMode::Extend : SelectMode::Replace>(
            context,
            std::bind(flags & SelectFlags::Reverse ? select_to_reverse : select_to,
                      _1, _2, key.key, params.count, flags & SelectFlags::Inclusive));
    }, "select to next char","enter char to select to");
}

static bool is_basic_alpha(Codepoint c)
{
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z');
}

void start_or_end_macro_recording(Context& context, NormalParams)
{
    if (context.input_handler().is_recording())
        context.input_handler().stop_recording();
    else
        on_next_key_with_autoinfo(context, KeymapMode::None,
                                 [](Key key, Context& context) {
            if (key.modifiers == Key::Modifiers::None and is_basic_alpha(key.key))
                context.input_handler().start_recording(tolower(key.key));
        }, "record macro", "enter macro name ");
}

void end_macro_recording(Context& context, NormalParams)
{
    if (context.input_handler().is_recording())
        context.input_handler().stop_recording();
}

void replay_macro(Context& context, NormalParams params)
{
    on_next_key_with_autoinfo(context, KeymapMode::None,
                             [params](Key key, Context& context) mutable {
        if (key.modifiers == Key::Modifiers::None and is_basic_alpha(key.key))
        {
            static bool running_macros[26] = {};
            const char name = tolower(key.key);
            const size_t idx = (size_t)(name - 'a');
            if (running_macros[idx])
                throw runtime_error("recursive macros call detected");

            ArrayView<String> reg_val = RegisterManager::instance()[name].values(context);
            if (not reg_val.empty())
            {
                running_macros[idx] = true;
                auto stop = on_scope_end([&]{ running_macros[idx] = false; });

                auto keys = parse_keys(reg_val[0]);
                ScopedEdition edition(context);
                do { exec_keys(keys, context); } while (--params.count > 0);
            }
        }
    }, "replay macro", "enter macro name");
}

template<Direction direction>
void jump(Context& context, NormalParams)
{
    auto jump = (direction == Forward) ?
                 context.jump_forward() : context.jump_backward();

    Buffer& buffer = const_cast<Buffer&>(jump.buffer());
    BufferManager::instance().set_last_used_buffer(buffer);
    if (&buffer != &context.buffer())
        context.change_buffer(buffer);
    context.selections() = jump;
}

void save_selections(Context& context, NormalParams)
{
    context.push_jump();
    context.print_status({ "saved " + to_string(context.selections().size()) +
                           " selections", get_face("Information") });
}

void align(Context& context, NormalParams)
{
    auto& selections = context.selections();
    auto& buffer = context.buffer();
    const CharCount tabstop = context.options()["tabstop"].get<int>();

    Vector<Vector<const Selection*>> columns;
    LineCount last_line = -1;
    size_t column = 0;
    for (auto& sel : selections)
    {
        auto line = sel.cursor().line;
        if (sel.anchor().line != line)
            throw runtime_error("align cannot work with multi line selections");

        column = (line == last_line) ? column + 1 : 0;
        if (column >= columns.size())
            columns.resize(column+1);
        columns[column].push_back(&sel);
        last_line = line;
    }

    const bool use_tabs = context.options()["aligntab"].get<bool>();
    for (auto& col : columns)
    {
        CharCount maxcol = 0;
        for (auto& sel : col)
            maxcol = std::max(get_column(buffer, tabstop, sel->cursor()), maxcol);
        for (auto& sel : col)
        {
            auto insert_coord = sel->min();
            auto lastcol = get_column(buffer, tabstop, sel->cursor());
            String padstr;
            if (not use_tabs)
                padstr = String{ ' ', maxcol - lastcol };
            else
            {
                auto inscol = get_column(buffer, tabstop, insert_coord);
                auto targetcol = maxcol - (lastcol - inscol);
                auto tabcol = inscol - (inscol % tabstop);
                auto tabs = (targetcol - tabcol) / tabstop;
                auto spaces = targetcol - (tabs ? (tabcol + tabs * tabstop) : inscol);
                padstr = String{ '\t', tabs } + String{ ' ', spaces };
            }
            buffer.insert(buffer.iterator_at(insert_coord), std::move(padstr));
        }
        selections.update();
    }
}

void copy_indent(Context& context, NormalParams params)
{
    int selection = params.count;
    auto& buffer = context.buffer();
    auto& selections = context.selections();
    Vector<LineCount> lines;
    for (auto sel : selections)
    {
        for (LineCount l = sel.min().line; l < sel.max().line + 1; ++l)
            lines.push_back(l);
    }
    if (selection > selections.size())
        throw runtime_error("invalid selection index");
    if (selection == 0)
        selection = context.selections().main_index() + 1;

    auto ref_line = selections[selection-1].min().line;
    auto line = buffer[ref_line];
    auto it = line.begin();
    while (it != line.end() and is_horizontal_blank(*it))
        ++it;
    const StringView indent = line.substr(0_byte, (int)(it-line.begin()));

    ScopedEdition edition{context};
    for (auto& l : lines)
    {
        if (l == ref_line)
            continue;

        auto line = buffer[l];
        ByteCount i = 0;
        while (i < line.length() and is_horizontal_blank(line[i]))
            ++i;
        buffer.erase(buffer.iterator_at(l), buffer.iterator_at({l, i}));
        buffer.insert(buffer.iterator_at(l), indent);
    }
}

void tabs_to_spaces(Context& context, NormalParams params)
{
    auto& buffer = context.buffer();
    const CharCount opt_tabstop = context.options()["tabstop"].get<int>();
    const CharCount tabstop = params.count == 0 ? opt_tabstop : params.count;
    Vector<Selection> tabs;
    Vector<String> spaces;
    for (auto& sel : context.selections())
    {
        for (auto it = buffer.iterator_at(sel.min()),
                  end = buffer.iterator_at(sel.max())+1; it != end; ++it)
        {
            if (*it == '\t')
            {
                CharCount col = get_column(buffer, opt_tabstop, it.coord());
                CharCount end_col = (col / tabstop + 1) * tabstop;
                tabs.push_back({ it.coord() });
                spaces.push_back(String{ ' ', end_col - col });
            }
        }
    }
    if (not tabs.empty())
        SelectionList{ buffer, std::move(tabs) }.insert(spaces, InsertMode::Replace);
}

void spaces_to_tabs(Context& context, NormalParams params)
{
    auto& buffer = context.buffer();
    const CharCount opt_tabstop = context.options()["tabstop"].get<int>();
    const CharCount tabstop = params.count == 0 ? opt_tabstop : params.count;
    Vector<Selection> spaces;
    for (auto& sel : context.selections())
    {
        for (auto it = buffer.iterator_at(sel.min()),
                  end = buffer.iterator_at(sel.max())+1; it != end;)
        {
            if (*it == ' ')
            {
                auto spaces_beg = it;
                auto spaces_end = spaces_beg+1;
                CharCount col = get_column(buffer, opt_tabstop, spaces_end.coord());
                while (*spaces_end == ' ' and (col % tabstop) != 0)
                {
                    ++spaces_end;
                    ++col;
                }
                if ((col % tabstop) == 0)
                    spaces.push_back({spaces_beg.coord(), (spaces_end-1).coord()});
                else if (*spaces_end == '\t')
                    spaces.push_back({spaces_beg.coord(), spaces_end.coord()});
                it = spaces_end;
            }
            else
                ++it;
        }
    }
    if (not spaces.empty())
        SelectionList{ buffer, std::move(spaces) }.insert("\t"_str, InsertMode::Replace);
}

void undo(Context& context, NormalParams)
{
    Buffer& buffer = context.buffer();
    size_t timestamp = buffer.timestamp();
    bool res = buffer.undo();
    if (res)
    {
        auto ranges = compute_modified_ranges(buffer, timestamp);
        if (not ranges.empty())
            context.set_selections(std::move(ranges));
        context.selections().avoid_eol();
    }
    else if (not res)
        context.print_status({ "nothing left to undo", get_face("Information") });
}

void redo(Context& context, NormalParams)
{
    using namespace std::placeholders;
    Buffer& buffer = context.buffer();
    size_t timestamp = buffer.timestamp();
    bool res = buffer.redo();
    if (res)
    {
        auto ranges = compute_modified_ranges(buffer, timestamp);
        if (not ranges.empty())
            context.set_selections(std::move(ranges));
        context.selections().avoid_eol();
    }

    else if (not res)
        context.print_status({ "nothing left to redo", get_face("Information") });
}

void exec_user_mappings(Context& context, NormalParams params)
{
    on_next_key_with_autoinfo(context, KeymapMode::None,
                             [params](Key key, Context& context) mutable {
        if (not context.keymaps().is_mapped(key, KeymapMode::User))
            return;

        auto mapping = context.keymaps().get_mapping(key, KeymapMode::User);
        ScopedEdition edition(context);
        exec_keys(mapping, context);
    }, "user mapping", "enter user key");
}

template<typename T>
class Repeated
{
public:
    constexpr Repeated(T t) : m_func(t) {}

    void operator() (Context& context, NormalParams params)
    {
        ScopedEdition edition(context);
        do { m_func(context, {0, params.reg}); } while(--params.count > 0);
    }
private:
    T m_func;
};

template<typename T>
constexpr Repeated<T> repeated(T func) { return Repeated<T>(func); }

template<typename Type, Direction direction, SelectMode mode = SelectMode::Replace>
void move(Context& context, NormalParams params)
{
    kak_assert(mode == SelectMode::Replace or mode == SelectMode::Extend);
    Type offset(std::max(params.count,1));
    if (direction == Backward)
        offset = -offset;
    auto& selections = context.selections();
    for (auto& sel : selections)
    {
        auto cursor = context.has_window() ? context.window().offset_coord(sel.cursor(), offset)
                                           : context.buffer().offset_coord(sel.cursor(), offset);

        sel.anchor() = mode == SelectMode::Extend ? sel.anchor() : cursor;
        sel.cursor() = cursor;
    }
    selections.avoid_eol();
    selections.sort_and_merge_overlapping();
}

KeyMap keymap =
{
    { 'h', { "move left", move<CharCount, Backward> } },
    { 'j', { "move down", move<LineCount, Forward> } },
    { 'k', { "move up",  move<LineCount, Backward> } },
    { 'l', { "move right", move<CharCount, Forward> } },

    { 'H', { "extend left", move<CharCount, Backward, SelectMode::Extend> } },
    { 'J', { "extend down", move<LineCount, Forward, SelectMode::Extend> } },
    { 'K', { "extend up", move<LineCount, Backward, SelectMode::Extend> } },
    { 'L', { "extend right", move<CharCount, Forward, SelectMode::Extend> } },

    { 't', { "select to next character", select_to_next_char<SelectFlags::None> } },
    { 'f', { "select to next character included", select_to_next_char<SelectFlags::Inclusive> } },
    { 'T', { "extend to next character", select_to_next_char<SelectFlags::Extend> } },
    { 'F', { "extend to next character included", select_to_next_char<SelectFlags::Inclusive | SelectFlags::Extend> } },
    { alt('t'), { "select to previous character", select_to_next_char<SelectFlags::Reverse> } },
    { alt('f'), { "select to previous character included", select_to_next_char<SelectFlags::Inclusive | SelectFlags::Reverse> } },
    { alt('T'), { "extend to previous character", select_to_next_char<SelectFlags::Extend | SelectFlags::Reverse> } },
    { alt('F'), { "extend to previous character included", select_to_next_char<SelectFlags::Inclusive | SelectFlags::Extend | SelectFlags::Reverse> } },

    { 'd', { "erase selected text", erase_selections } },
    { 'c', { "change selected text", change } },
    { 'i', { "insert before selected text", enter_insert_mode<InsertMode::Insert> } },
    { 'I', { "insert at line begin", enter_insert_mode<InsertMode::InsertAtLineBegin> } },
    { 'a', { "insert after selected text", enter_insert_mode<InsertMode::Append> } },
    { 'A', { "insert at line end", enter_insert_mode<InsertMode::AppendAtLineEnd> } },
    { 'o', { "insert on new line below", enter_insert_mode<InsertMode::OpenLineBelow> } },
    { 'O', { "insert on new line above", enter_insert_mode<InsertMode::OpenLineAbove> } },
    { 'r', { "replace with character", replace_with_char } },

    { 'g', { "go to location", goto_commands<SelectMode::Replace> } },
    { 'G', { "extend to location", goto_commands<SelectMode::Extend> } },

    { 'v', { "move view", view_commands } },

    { 'y', { "yank selected text", yank } },
    { 'p', { "paste after selected text", repeated(paste<InsertMode::Append>) } },
    { 'P', { "paste before selected text", repeated(paste<InsertMode::Insert>) } },
    { alt('p'), { "paste every yanked selection after selected text", paste_all<InsertMode::Append> } },
    { alt('P'), { "paste every yanked selection before selected text", paste_all<InsertMode::Insert> } },
    { 'R', { "replace selected text with yanked text", paste<InsertMode::Replace> } },

    { 's', { "select regex matches in selected text", select_regex } },
    { 'S', { "split selected text on regex matches", split_regex } },
    { alt('s'), { "split selected text on line ends", split_lines } },

    { '.', { "repeat last insert command", repeat_last_insert } },

    { '%', { "select whole buffer", [](Context& context, NormalParams) { select_buffer(context.selections()); } } },

    { ':', { "enter command prompt", command } },
    { '|', { "pipe each selection through filter and replace with output", pipe<true> } },
    { alt('|'), { "pipe each selection through command and ignore output", pipe<false> } },
    { '!', { "insert command output", insert_output<InsertMode::Insert> } },
    { alt('!'), { "append command output", insert_output<InsertMode::Append> } },

    { ' ', { "remove all selection except main", [](Context& context, NormalParams p) { keep_selection(context.selections(), p.count ? p.count-1 : context.selections().main_index()); } } },
    { alt(' '), { "remove main selection", [](Context& context, NormalParams p) { remove_selection(context.selections(), p.count ? p.count-1 : context.selections().main_index()); } } },
    { ';', { "reduce selections to their cursor", [](Context& context, NormalParams) { clear_selections(context.selections()); } } },
    { alt(';'), { "swap selections cursor and anchor", [](Context& context, NormalParams) { flip_selections(context.selections()); } } },

    { 'w', { "select to next word start", repeated(make_select<SelectMode::Replace>(select_to_next_word<Word>)) } },
    { 'e', { "select to next word end", repeated(make_select<SelectMode::Replace>(select_to_next_word_end<Word>)) } },
    { 'b', { "select to prevous word start", repeated(make_select<SelectMode::Replace>(select_to_previous_word<Word>)) } },
    { 'W', { "extend to next word start", repeated(make_select<SelectMode::Extend>(select_to_next_word<Word>)) } },
    { 'E', { "extend to next word end", repeated(make_select<SelectMode::Extend>(select_to_next_word_end<Word>)) } },
    { 'B', { "extend to prevous word start", repeated(make_select<SelectMode::Extend>(select_to_previous_word<Word>)) } },

    { alt('w'), { "select to next WORD start", repeated(make_select<SelectMode::Replace>(select_to_next_word<WORD>)) } },
    { alt('e'), { "select to next WORD end", repeated(make_select<SelectMode::Replace>(select_to_next_word_end<WORD>)) } },
    { alt('b'), { "select to prevous WORD start", repeated(make_select<SelectMode::Replace>(select_to_previous_word<WORD>)) } },
    { alt('W'), { "extend to next WORD start", repeated(make_select<SelectMode::Extend>(select_to_next_word<WORD>)) } },
    { alt('E'), { "extend to next WORD end", repeated(make_select<SelectMode::Extend>(select_to_next_word_end<WORD>)) } },
    { alt('B'), { "extend to prevous WORD start", repeated(make_select<SelectMode::Extend>(select_to_previous_word<WORD>)) } },

    { alt('l'), { "select to line end", repeated(make_select<SelectMode::Replace>(select_to_eol)) } },
    { alt('L'), { "extend to line end", repeated(make_select<SelectMode::Extend>(select_to_eol)) } },
    { alt('h'), { "select to line begin", repeated(make_select<SelectMode::Replace>(select_to_eol_reverse)) } },
    { alt('H'), { "extend to line begin", repeated(make_select<SelectMode::Extend>(select_to_eol_reverse)) } },

    { 'x', { "select line", repeated(make_select<SelectMode::Replace>(select_line)) } },
    { 'X', { "extend line", repeated(make_select<SelectMode::Extend>(select_line)) } },
    { alt('x'), { "extend selections to whole lines", make_select<SelectMode::Replace>(select_lines) } },
    { alt('X'), { "crop selections to whole lines", make_select<SelectMode::Replace>(trim_partial_lines) } },

    { 'm', { "select to matching character", make_select<SelectMode::Replace>(select_matching) } },
    { 'M', { "extend to matching character", make_select<SelectMode::Extend>(select_matching) } },

    { '/', { "select next given regex match", search<SelectMode::Replace, Forward> } },
    { '?', { "extend with next given regex match", search<SelectMode::Extend, Forward> } },
    { alt('/'), { "select previous given regex match", search<SelectMode::Replace, Backward> } },
    { alt('?'), { "extend with previous given regex match", search<SelectMode::Extend, Backward> } },
    { 'n', { "select next current search pattern match", search_next<SelectMode::Replace, Forward> } },
    { 'N', { "extend with next current search pattern match", search_next<SelectMode::Append, Forward> } },
    { alt('n'), { "select previous current search pattern match", search_next<SelectMode::Replace, Backward> } },
    { alt('N'), { "extend with previous current search pattern match", search_next<SelectMode::Append, Backward> } },
    { '*', { "set search pattern to main selection content", use_selection_as_search_pattern<true> } },
    { alt('*'), { "set search pattern to main selection content, do not detect words", use_selection_as_search_pattern<false> } },

    { 'u', { "undo", undo } },
    { 'U', { "redo", redo } },

    { alt('i'), { "select inner object", select_object<ObjectFlags::ToBegin | ObjectFlags::ToEnd | ObjectFlags::Inner> } },
    { alt('a'), { "select whole object", select_object<ObjectFlags::ToBegin | ObjectFlags::ToEnd> } },
    { '[', { "select to object start", select_object<ObjectFlags::ToBegin> } },
    { ']', { "select to object end", select_object<ObjectFlags::ToEnd> } },
    { '{', { "extend to object start", select_object<ObjectFlags::ToBegin, SelectMode::Extend> } },
    { '}', { "extend to object end", select_object<ObjectFlags::ToEnd, SelectMode::Extend> } },
    { alt('['), { "select to inner object start", select_object<ObjectFlags::ToBegin | ObjectFlags::Inner> } },
    { alt(']'), { "select to inner object end", select_object<ObjectFlags::ToEnd | ObjectFlags::Inner> } },
    { alt('{'), { "extend to inner object start", select_object<ObjectFlags::ToBegin | ObjectFlags::Inner, SelectMode::Extend> } },
    { alt('}'), { "extend to inner object end", select_object<ObjectFlags::ToEnd | ObjectFlags::Inner, SelectMode::Extend> } },

    { alt('j'), { "join lines", join_lines } },
    { alt('J'), { "join lines and select spaces", join_lines_select_spaces } },

    { alt('k'), { "keep selections matching given regex", keep<true> } },
    { alt('K'), { "keep selections not matching given regex", keep<false> } },
    { '$', { "pipe each selection through shell command and keep the ones whose command succeed", keep_pipe } },

    { '<', { "deindent", deindent<true> } },
    { '>', { "indent", indent<false> } },
    { alt('>'), { "indent, including empty lines", indent<true> } },
    { alt('<'), { "deindent, not including incomplete indent", deindent<false> } },

    { ctrl('i'), { "jump forward in jump list",jump<Forward> } },
    { ctrl('o'), { "jump backward in jump list", jump<Backward> } },
    { ctrl('s'), { "push current selections in jump list", save_selections } },

    { alt('r'), { "rotate main selection", rotate_selections } },
    { alt('R'), { "rotate selections content", rotate_selections_content } },

    { 'q', { "replay recorded macro", replay_macro } },
    { 'Q', { "start or end macro recording", start_or_end_macro_recording } },

    { Key::Escape, { "end macro recording", end_macro_recording } },

    { '`', { "convert to lower case in selections", for_each_char<to_lower> } },
    { '~', { "convert to upper case in selections", for_each_char<to_upper> } },
    { alt('`'),  { "swap case in selections", for_each_char<swap_case> } },

    { '&', { "align selection cursors", align } },
    { alt('&'), { "copy indentation", copy_indent } },

    { '@', { "convert tabs to spaces in selections", tabs_to_spaces } },
    { alt('@'), { "convert spaces to tabs in selections", spaces_to_tabs } },

    { 'C', { "copy selection on next lines", copy_selections_on_next_lines<Forward> } },
    { alt('C'), { "copy selection on previous lines", copy_selections_on_next_lines<Backward> } },

    { ',', { "user mappings", exec_user_mappings } },

    { Key::Left,  { "move left", move<CharCount, Backward> } },
    { Key::Down,  { "move down", move<LineCount, Forward> } },
    { Key::Up,    { "move up", move<LineCount, Backward> } },
    { Key::Right, { "move right", move<CharCount, Forward> } },

    { ctrl('b'), { "scroll one page up", scroll<Key::PageUp> } },
    { ctrl('f'), { "scroll one page down", scroll<Key::PageDown> } },

    { Key::PageUp,   { "scroll one page up", scroll<Key::PageUp> } },
    { Key::PageDown, { "scroll one page down", scroll<Key::PageDown> } },
};

}
