-- Fixture for the script-binding test in tests/unit_te.c. Uses Lua-side
-- assert() so the test exercises te.* through real Lua code; a failed
-- assert surfaces as a "lua error" echo instead of the expected final
-- buffer/echo state, which the C test checks for.
assert(te.text() == "", "expected empty buffer at start")

te.insert("hello")
assert(te.text() == "hello", "te.insert did not update the buffer")
assert(te.cursor() == 5, "cursor should follow the insert")

te.action("undo")
assert(te.text() == "", "te.action('undo') should revert the insert")

-- Registering bindings should not error, for both a function handler and an
-- existing action name resolved through COMMANDS -- top-level and leader
-- chords alike.
te.bind("j", "ctrl-shift", function() te.insert("bound!") end)
te.bind("u", "ctrl", "undo")
te.bind_leader("d", "any", function() te.insert("bound-leader!") end)
te.bind_leader("s", "ctrl", "save")

te.echo("hello from init.lua")
