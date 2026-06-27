_pak()
{
    local cur prev command
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    if [[ $COMP_CWORD -eq 1 ]]; then
        COMPREPLY=( $(compgen -W "make update list extract unpack cat info delete rename repack check --help" -- "$cur") )
        return
    fi

    command="${COMP_WORDS[1]}"
    case "$prev" in
        -C|--level|--exclude)
            COMPREPLY=( $(compgen -f -- "$cur") )
            return
            ;;
    esac

    case "$command" in
        make|update)
            COMPREPLY=( $(compgen -W "--compress --level --paths --exclude --no-pakignore --" -- "$cur") $(compgen -f -- "$cur") )
            ;;
        repack)
            COMPREPLY=( $(compgen -W "--compress --level --store" -- "$cur") $(compgen -f -- "$cur") )
            ;;
        list)
            COMPREPLY=( $(compgen -W "--long --full-name" -- "$cur") $(compgen -f -- "$cur") )
            ;;
        extract|unpack)
            COMPREPLY=( $(compgen -W "-C --overwrite --skip-existing" -- "$cur") $(compgen -f -- "$cur") )
            ;;
        cat|info|delete|rename|check)
            COMPREPLY=( $(compgen -f -- "$cur") )
            ;;
        *)
            COMPREPLY=( $(compgen -W "make update list extract unpack cat info delete rename repack check" -- "$cur") )
            ;;
    esac
}
complete -F _pak pak
