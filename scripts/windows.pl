% Window management, in Prolog rather than as static C functions -- the
% structural tree work (split/close a pane, keep every leaf's `buf` pointer
% valid) stays native (script.c's te_window_* predicates, wrapping main.c's
% Window tree), since that's memory-safety-bearing work no different in
% kind from buffer create/kill (src/buffers.pl). This file is the thin
% policy on top: which window becomes selected after each operation.

% Splitting selects the *new* pane (not the one just split, unlike some
% other editors' default) -- a deliberate choice so the split immediately
% puts you to work in the new half.
split_right :- te_selected_window(W), te_window_split(W, right, New), te_select_window(New).
split_below :- te_selected_window(W), te_window_split(W, below, New), te_select_window(New).

% te_window_close/1 already repoints the selected window itself if it was
% the one just closed (main.c's windowClose), so there's nothing extra to
% decide here.
delete_window :- te_selected_window(W), te_window_close(W).
delete_other_windows :- te_selected_window(W), te_window_delete_others(W).

% Cycles to the next pane in reading order (wrapping), the window
% equivalent of src/buffers.pl's next_buffer/prev_buffer cycling.
other_window :-
    te_window_list(Ids), te_selected_window(W),
    nth0(Idx, Ids, W), !,
    length(Ids, Len),
    NewIdx is (Idx + 1) mod Len,
    nth0(NewIdx, Ids, NewW),
    te_select_window(NewW).
