-- Fixture for the lifecycle-hook test in tests/unit_te.c.
te.on("post-save", function()
    te.echo("post-save fired")
end)
te.on("post-open", function()
    te.echo("post-open fired")
end)
