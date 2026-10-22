# recc

recc is a CLI tool for statically compiling nodosumi's IR format for optimization and testing purposes.

# Generating IR

While running nodosumi, open the debug toolbar and select `SH4 -> start block dump`. This will start dumping out every block as it is compiled to `$HOME/.nodosumi`.

# Compiling IR

```
recc [options] <path to file or directory>
```

### Options
```
           --pass  Comma-separated list of passes to run  [default: lse, dce, ra]
          --stats  Print pass stats                       [default: 1]
--print_after_all  Print IR after each pass               [default: 1]
```
