# Разработка плагинов

> API плагинов — **экспериментальный Python API**; `PLUGIN_API` — версия контракта (сейчас 2, совместим с API 1), а не бинарный ABI.
> Файлы `plugins/*_page.py` в репозитории — примеры API (не продукты), их production-готовность не гарантируется.

## 1. Загрузка и каталоги

```text
plugins/
  hello.py                однофайловый плагин
  multi/                  плагин-каталог
    plugin.py             точка входа (приоритетнее __init__.py)
    __init__.py
    helper.py
```

- Загрузчик просматривает только **верхний уровень** `plugins\`;
- однофайловый: ключ/ключ отключения = имя файла;
- каталог: точка входа `<dir>\plugin.py` или `<dir>\__init__.py`; ключ = имя каталога;
- имя модуля = имя точки входа (`.`/`-` заменяются на `_`); каталог загружается как пакет, доступно `from . import helper`;
- `plugins.disabled` хранит отключения; отключённый плагин не загружается, причина показывается;
- межплагинный import разрешён только после присваивания `PLUGIN_INFO`; цель должна быть указана в `PLUGIN_INFO["depends"]`; в runtime-колбэках межплагинный import запрещён, используйте `api.services.call`.

## 2. Минимальный плагин

```python
PLUGIN_API = 2
PLUGIN_INFO = {
    "name": "Hello",
    "version": "1.0",
    "description": "Минимальный пример",
}

def register(api):
    api.log("Hello: зарегистрирован")

    def build():
        api.ui.label(None, "Привет, это страница плагина")
        api.ui.button(None, "Нажми", lambda: api.log("clicked"))

    api.register_tool("hello", build, 100, "", "Пример", "Hello")
```

- `register(api)` вызывается в главном потоке UI; не выполняйте там тяжёлую работу;
- `register_tool(name, build, order=0, replaces="", subtitle="", title="")`:
  - `name` уникально; `order` задаёт порядок вкладки;
  - `replaces` заменяет страницу с указанным id; пусто — новая страница;
  - `build()` также вызывается в главном потоке UI и может вернуть `None`;
- необязательный `headless(api, args)` используется `xrkit_cli --py-plugin-call`; он не должен обращаться к виджетам.

## 3. API и потоки

| Часть | Назначение | Поток |
|---|---|---|
| `api.ui` | Виджеты, диалоги, оболочка задач, дерево/список | Почти всё только в главном потоке UI |
| `api.engine` | Распаковка/упаковка, текст, шрифты, XML, видео, патчи | Можно из worker |
| `api.host` | Настройки, процессы, пути, HTTP | См. ниже |

### 3.1 Основные методы UI

- Виджеты: `label` / `button` / `row` / `checkbox` / `entry` / `text` / `combo` / `list` / `tree` / `tabs` / `drop_box`;
- Чтение/запись: `set_text` / `get_text` / `set_enabled` / `set_checked` / `checkbox_state` / `combo_value` / `combo_set_items` / `set_progress` / `progress` / `log`;
- Дерево: `tree_set_columns` / `tree_set_root` / `tree_set_children` / `tree_on_expand` / `tree_on_check` / `tree_set_checked` / `tree_checked` / `tree_set_filter` / `tree_filter_hits` / `tree_on_click` / `tree_on_activate` / `tree_on_action` / `tree_select` / `tree_selected_key` / `tree_selected_keys` / `tree_set_expanded` / `tree_expanded_keys`;
- Список: `list_set_row_count` / `list_set_row_text` / `list_selected_rows` / `list_set_multi`;
- Компоновка: `pack` / `split` / `split_add_pane`;
- Меню: `button_on_menu` / `context_menu`;
- Диалоги: `ask_directory` / `ask_open_file` / `ask_save_file` / `ask_yes_no` / `pick_font` / `copy_text` / `open_path`;
- Задачи: `task_panel` / `task_run` / `task_status` / `task_cancel` / `task_cancel_requested` / `task_on_message` / `on_ui_thread`.

Полный список — в строке `UIFACADE Ui=` команды `xrkit_cli --ui-facade`.

### 3.2 Контракт потоков

- В worker разрешены только `ui.task_cancel_requested`, `ui.on_ui_thread`, `ui.post`; остальные `api.ui.*` отклоняются с Warn;
- `ui.post(fn, *args)` можно вызывать из любого потока, колбэк выполняется в UI-потоке; после уничтожения страницы сначала проверьте `ui.alive`;
- Правильный путь: главный поток читает значения → чистые данные → `task_run` worker вызывает движок → `message` возвращает в главный поток → `task_on_message` работает с виджетами;
- `api.engine.*` можно вызывать из worker; `api.host.pref_get/pref_set` рекомендуется из главного потока;
- Если главный поток UI не подаёт heartbeat 30 секунд, watchdog пишет `logs/ui_watchdog.log` и завершает процесс (fail-fast); долгие операции — в worker.

### 3.3 Жизненный цикл

```python
def shutdown(api):
    ...  # остановить потоки/процессы/таймеры

def register(api):
    api.on_unload(lambda: ...)
```

- Перед reload, обновлением, отключением и выходом вызывается `shutdown(api)`, затем колбэки `on_unload`; всё в главном потоке/GIL;
- В колбэках не трогайте виджеты; только очистка потоков/процессов/файлов/модулей;
- Динамически импортированные vendor-модули регистрируйте через `api.register_module`;
- Python-поток нельзя убить принудительно; `shutdown` кооперативный; плагин должен сам завершить worker.

## 4. `api.host`

- `pref_get(section, key, default)` / `pref_set(...)`: запись в `xrkit_user.ltx`;
- `data_dir()` / `cache_dir()`: постоянный/очищаемый каталог плагина;
- `run_process(options)`: `program/args/working_dir/on_stdout/on_stderr/should_stop/cancel_wait_ms`;
- `start_process/stop_process/process_alive`: долгоживущий процесс без каналов, логи в файл, kill-on-close;
- `http_get` / `http_post`: возвращают `ok/status/body/error/bytes/truncated`; блокирующая часть освобождает GIL; `http_post` по умолчанию ограничен 8 MiB (при превышении `truncated=true` + `error`);
  **Общий сетевой переключатель всегда главный для плагинов**: при выключенной сети появляется запрос (`[network] plugin_prompt=1`, по умолчанию включён); разрешение действует только на этот запрос (кэш owner+host 10 минут); CLI печатает предупреждение и читает `y/N`; если stdin не терминал (CI/pipe/self-test), запрос всегда отклоняется;
- `http_download(url_or_urls, path, timeout_sec=300, max_bytes=8GB, wait_seconds=-1, workers=4, sha256="")`: синхронная загрузка через `DownloadManager`; `wait_seconds=0` сразу возвращает `{job}` (можно опрашивать/отменять), `sha256` проверяет менеджер;
- `http_download_async(url_or_urls, path, workers=4, timeout_sec=300, max_bytes=8GB, prefer_direct=False, sha256="")`: сразу возвращает job; части докачки переиспользуются по `sha256 + total + segments` (**смена зеркала поддерживается**; без sha привязка к url);
- `http_download_status(job)`: `found/finished/ok/cancelled/done/total/error/path/url`;
- `http_cancel(job)`: устанавливает отмену; блокирующий WinHTTP ждёт до текущего таймаута; части сохраняются;
- При выгрузке/reload хоста все асинхронные загрузки плагина отменяются по owner;
- Синхронные HTTP-вызовы должны быть в `task_run` worker; вызов из UI-потока отклоняется;
- `api.supports("host.http_async")` / `"host.http_download_async"` проверяют возможности; начиная с BETA.270 есть `host.http.concurrent` / `host.http.plugin_prompt` / `host.download.cross_source_resume` / `host.download.sha256` / `host.download.submit` / `host.capabilities`;
- `api.host.capabilities()` возвращает структурированную таблицу (`http.gil` / `http.network_switch` / `download.cross_source_resume` / `download.sha256` / `download.sync_submit` и т.д.), по ней новый хост решает, нужен ли собственный канал.

## 5. Условия `when`

- falsy ⇒ всегда истина;
- dict ⇒ сравнение строковых ключей/значений; нестроковые значения не совпадают; `None` означает отсутствие ключа;
- вызываемый объект ⇒ получает dict, истинное значение — совпадение; исключение считается несовпадением;
- остальные истинные значения (включая строку) — всегда истина.

## 6. Drag & drop

`ui.drop_box(parent, on_file, placeholder, width)` принимает локальные файлы; колбэк сейчас получает только первый путь.

## 7. Сеть и SQLite

- В архив входят Python SSL и sqlite3; можно использовать `urllib` / `http.client` / `sqlite3`;
- «Инструменты → Сеть» ограничивает только обновление хоста, но не плагины; если нужно уважать настройку, читайте `api.host.pref_get("network","enabled","1")`;
- Сетевые операции — в worker, со своими таймаутами и лимитами;
- Файлы SQLite — в каталоге плагина; не открывайте недоверенные базы; не используйте одно соединение из разных потоков.

## 8. Отладка

| Средство | Использование |
|---|---|
| `xrkit_cli --py-plugins <dir>` | Загрузка/статистика/ошибки/причины отключения |
| `xrkit_cli --ui-facade` | Реально экспортированные имена UI |
| `logs/log_*.txt` | Журнал хоста; `api.log` пишет строку |
| `xrkit_plugin_manager.exe` | Загруженные/отключённые/обновление |

Ограничения:

- Один процесс: падение, побочные эффекты import и бесконечный цикл влияют на хост;
- При отсутствии зависимости из `PLUGIN_INFO.requires` плагин автоматически отключается;
- Обновление плагина — резервная копия всего каталога и замена, при ошибке откат; прежнее состояние отключения сохраняется.

## 9. Безголовый вход

```python
def headless(api, args):
    return {"ok": True, "value": args.get("path", "")}
```

```bat
xrkit_cli --py-plugin-call plugins {"path":"E:\\x"}
```

Нельзя обращаться к `api.ui`; возвращаемое значение сериализуется в JSON.

## 10. Точки расширения

- `register_decryptor(check, decrypt)`: `check` должен быть дешёвым; `decrypt` — в worker; открытый текст пишется во временный изолированный каталог, не в источник;
- `register_format(name, handler)`: объявление распознавания, разбор выполняет плагин;
- `register_compare_mode(spec, compare)`: возвращает dict; неизвестные ключи попадают в `unhandled_keys`;
- Проверка возможностей: `api.supports("api.register_decryptor")` и т.п.
