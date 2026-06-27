#compdef pak

local -a commands common_make extract_opts
commands=(
  'make:write a new archive'
  'update:add or replace files in an archive'
  'list:list archive entries'
  'extract:extract archive entries'
  'unpack:extract archive entries'
  'cat:write one entry to stdout'
  'info:show archive summary'
  'verify:check archive data'
  'test:check archive data'
)
common_make=(
  '--compress[compress entries when useful]'
  '--level[set deflate level]:level:(0 1 2 3 4 5 6 7 8 9 10)'
  '--paths[keep relative paths]'
  '--exclude[skip pattern]:pattern:'
  '--no-pakignore[do not read .pakignore]'
)
extract_opts=(
  '-C[extract into directory]:directory:_files -/'
  '--overwrite[replace existing files]'
  '--skip-existing[skip existing files]'
)

_arguments -C \
  '1:command:->command' \
  '*::arg:->args'

case $state in
  command)
    _describe 'command' commands
    ;;
  args)
    case $words[2] in
      make|update)
        _arguments $common_make '*:file:_files'
        ;;
      list)
        _arguments '--long[detailed list output]' '*:archive:_files -g "*.pak"'
        ;;
      extract|unpack)
        _arguments $extract_opts '*:entry or archive:_files'
        ;;
      cat|info|verify|test)
        _files
        ;;
    esac
    ;;
esac
