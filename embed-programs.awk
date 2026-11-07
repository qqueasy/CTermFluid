# Embed seed copies of the .conf files; runtime still loads editable files.
BEGIN {
    print "typedef struct { const char *name, *contents; } BundledProgram;"
    print "static const BundledProgram bundled_programs[] = {"
}
FNR == 1 {
    if (NR > 1) print "},"
    name = FILENAME
    sub(/^.*\//, "", name)
    sub(/\.conf$/, "", name)
    printf "{\"%s\",\n", name
}
{
    line = $0
    gsub(/\\/, "\\\\\\\\", line)
    gsub(/"/, "\\\"", line)
    printf "\"%s\\n\"\n", line
}
END {
    print "}"
    print "};"
}
