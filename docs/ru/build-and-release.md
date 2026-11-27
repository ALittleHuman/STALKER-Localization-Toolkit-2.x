# Сборка и выпуск

## Установка и откат

Установка: перенесите каталог сборки на целевую машину; запускайте `xrkit_app.exe`/`xrkit_cli.exe`. Инструмент не пишет в реестр и не устанавливает службы.

Откат: сохраните каталог предыдущей версии и просто верните его; при ошибке обновления `xrkit_updater.exe` выполняет откат транзакции.

Важно: по умолчанию распаковка/упаковка пишет в **каталог источника** (движок `resolve_output_root`); если не изменить путь, результат попадёт в каталог исходного пакета. Инструмент не устанавливает и не изменяет файлы игры, но и не меняет путь вывода за пользователя.

## 1. Требования

- Windows 10/11 x64;
- Visual Studio 2022 Build Tools (рабочая нагрузка C++);
- CMake ≥ 3.21, Ninja;
- Qt 6.8.3 MSVC 2022 x64;
- Python 3.14 (нужен для сборки; в архив входит embeddable Python).

Пути можно переопределить переменными:

```text
XRKIT_VS       корень VS Build Tools
XRKIT_QT       корень Qt
XRKIT_PYTHON   Python для сборки
```

## 2. Сборка

```bat
scripts\build.ps1                          :: msvc-release: CLI + движок
scripts\build.ps1 -Preset msvc-shell       :: GUI + xrkit_smoke
```

Результаты:

```text
build\msvc-release\tools\cli\xrkit_cli.exe
build\msvc-shell\src\shell\xrkit_app.exe
build\msvc-shell\src\shell\xrkit_smoke.exe
```

## 3. smoke отделён от приложения

Начиная с RC2:

- `xrkit_app.exe` **больше не содержит** `smoke.cpp` / `--smoke`;
- `xrkit_smoke.exe` — отдельная офскрин-самопроверка со своими ресурсами версии и manifest;
- она используется только для сборки, CI и временной самопроверки пакета и **не входит в релиз**;
- `package.ps1` временно копирует `xrkit_smoke.exe` при упаковке и удаляет после проверки;
- итоговая строка:

```text
SHELL-SMOKE ok checks=N failed=0 not_covered=0
```

## 4. Упаковка

Официальный вход — CLI:

```bat
xrkit_cli --build-package version.json 2.0.0 BETA <номер_канала>
xrkit_cli --build-package version.json 2.0.0 BETA <номер_канала> --prebuilt
```

- `--prebuilt` только считает sha/упаковывает/завершает, без пересборки;
- `scripts\package.ps1` вызывается из `--build-package`; при ручном вызове нужно передать все параметры:

```bat
scripts\package.ps1 -BuildIndex <номер_канала> -VersionString "x.y.z.<build>" -ExeSha256 <sha256>
```

- результат — каталог, без zip; расположение:

```text
dist\<основная_версия>\<метка>\<номер>\
```

- `platforms\` содержит и `qwindows.dll`, и `qoffscreen.dll`; `qoffscreen.dll` остаётся в пакете постоянно, а не удаляется после самопроверки;
- только каталог, без zip;
- итоговая строка самопроверки: `SHELL-SMOKE ok checks=N failed=0`;
- в `version.txt` пишутся `index` (номер канала) и `build` (глобальный номер сборки);
- `xrkit_smoke.exe` в готовом пакете должен отсутствовать; smoke существует только во время проверки;
- упаковка запускает smoke, Python-хост, цепочку плагинов и IX-Ray selftest;
- старые каталоги **не удаляются автоматически**; опубликованные старые пакеты не трогайте.

## 5. Выпуск

```bat
scripts\release.ps1 -Index <номер_канала> -Version 2.0.0 -Mark BETA
```

Скрипт выполняет четыре шага:

1. `--write-version` увеличивает глобальный `build_counter`, пишет `version.h` / `.rc` / `version.json.current`;
2. собирает `msvc-shell` и `msvc-release`;
3. выполняет `--build-package --prebuilt`;
4. проверяет `version.txt`, `--version` встроенного CLI, VERSIONINFO приложения и sha256.

Результат:

```text
dist\2.0.0\BETA\<номер_канала>
```

**Скрипт только собирает/упаковывает/проверяет, но не публикует.**

## 6. Модель версий C

- единственный источник порядка — `build_counter` в `version.json`, +1 на каждую сборку;
- версия стабильной сборки: `x.y.z`;
- версия тестовой сборки: `x.y.z.<build>`;
- канал (ALPHA/BETA/RC) — только метка, в версию не входит;
- путь dist: `dist\x.y.z\<метка>\<номер_канала>`;
- тестовые сборки сравниваются по build; стабильная версия выше тестовой той же версии.

Пример:

```text
BETA.270 + build 530 ⇒ 2.0.0.530
RC.2     + build 514 ⇒ 2.0.0.514
```

## 7. Обновления GitHub / npm

- GitHub: BETA/RC обязательно создаются как **Prerelease**; обычный Release — только для стабильных версий;
- в одном Release должен быть один zip; имя asset — `<build>.zip`, tag — строка версии;
- workflow npm: `.github/workflows/publish-update-npm.yml`;
- четырёхчастная версия модели C не является допустимым npm semver: workflow преобразует её в `x.y.z-<build>`, путь asset остаётся `full/x.y.z.<build>.zip`;
- клиент пробует точную npm-версию и резервный URL `x.y.z-<build>`;
- BETA.269 опубликована в GitHub/npm; BETA.270 собрана только локально и **пока не опубликована**; RC2 `2.0.0.514` — существующий Prerelease.

## 8. CI

```bat
third_party\python-embed\python.exe tests\run_ci.py --fast
```

- fast: публичный CI, отсутствие внешних предпосылок — предупреждение;
- release: любой SKIP — FAIL;
- полезные отдельные проверки: `check_release_criterion.py`, `check_version_consistency.py`, `check_fontpack_ref.py`, `check_host_update_poll.py`;
- при ручной сборке обязательно задайте `XRKIT_PYTHON`, иначе префикс `/showIncludes` может не определиться и изменение заголовка не вызовет пересборку.

## 9. Частые проблемы

| Симптом | Что делать |
|---|---|
| `vcvars 环境导出失败` | Проверьте `VC\Auxiliary\Build\vcvars64.bat` в `XRKIT_VS` |
| `LNK1104` не открывает exe | Закройте запущенные `xrkit_app`/`xrkit_plugin_manager`/`xrkit_cli` |
| Ошибка самопроверки пакета | Смотрите `logs`; часто отсутствует `qoffscreen.dll` или runtime Python |
| smoke завершается с `0xC0000409` | В каталоге сборки нет ресурсов упаковки; используйте самопроверку через `--build-package` |
| build встроенного CLI не совпадает с version.txt | Проверьте `build=` в `version.txt` и что `--write-version` записал изменения |
