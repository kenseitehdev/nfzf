1. memory leak in strip_ansi
2. buffer overflow risk ub parse_ssh_path
3. potential NULL pointer dereference in navigate_directory
4. race condition in fuzzy state uses global var, not thread safe
5. unchecked return in getcwd, add_line, and realpath
6. directory mode hidden file toggle clears query
7. score +=5 *consecutive, potential error overflow
8. missing error handling in ssh commands
9. dir path concat bug risk in snprintf(full_path, sizeof(full_path), "%s/%s", st->current_dir, output);
10. incomplete ansi escape handling
11. inconsistent error messages
12. no validation of max line len
13. In main(), several early return paths don't free allocated memory