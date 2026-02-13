1. buffer overflow risk in parse_ssh_path
2. potential NULL pointer dereference in navigate_directory
3. race condition in fuzzy state uses global var, not thread safe
4. unchecked return in getcwd, add_line, and realpath
5. directory mode hidden file toggle clears query
6. score +=5 *consecutive, potential error overflow
7. missing error handling in ssh commands
8. dir path concat bug risk in snprintf(full_path, sizeof(full_path), "%s/%s", st->current_dir, output);
9. inconsistent error messages
10. no validation of max line len
11. In main(), several early return paths don't free allocated memory