"""App definition and lazy command registration."""

from cyclopts import App

app = App(
    help="EarthBound/Mother 2 ROM asset toolkit",
)

# Lazy-load all commands — modules are only imported when a command is invoked.
app.command("ebtools.cli.extract:extract")
app.command("ebtools.cli.embed:embed_registry")
app.command("ebtools.cli.pack:pack_app", name="pack")
app.command("ebtools.cli.generate:generate_app", name="generate")
app.command("ebtools.cli.analyze_dump:dump_app", name="analyze-dump")
app.command("ebtools.cli.hallz:hallz_app", name="hallz")
app.command("ebtools.cli.pack_all:pack_all", name="pack-all")
app.command("ebtools.cli.coverage:coverage_app", name="coverage")
app.command("ebtools.cli.analyze_flags:analyze_flags_app", name="analyze-flags")
