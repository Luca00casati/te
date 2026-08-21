% Fixture for the lifecycle-hook test in tests/unit_te.c.
hook(post_save, notify_saved).
hook(post_open, notify_opened).

notify_saved :- te_echo("post-save fired").
notify_opened :- te_echo("post-open fired").
