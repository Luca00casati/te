% Insertion/deletion/clipboard/whole-line editing, in Prolog rather than as
% static C functions -- the same split as src/movement.pl and src/search.pl:
% the raw buffer work stays native (te_insert/1, te_apply_replace/3 -- both
% already existed, for typing and search/replace respectively -- plus
% te_byte_at/2, te_buffer_range/3, te_copy_range/2, te_clipboard_get/1 and
% te_tab/1, added alongside this file in src/script.c), and this is the
% decision logic on top: what to insert/delete/copy and where, and how the
% cursor should land afterward.
%
% te_apply_replace always leaves the cursor at the end of what it just
% spliced in (mirroring ordinary typing). Several ops here want the cursor
% somewhere else -- e.g. open_line_above lands on the new blank line above,
% not after the newline it inserted -- so those capture the pre-edit cursor
% and call te_set_cursor afterward to override it, same as the old C did by
% assigning `cursor`/`anchor` again right after its own edit() call.

has_sel :- te_anchor(A), te_cursor(C), A =\= C.
sel_min(Min) :- te_anchor(A), te_cursor(C), Min is min(A, C).
sel_max(Max) :- te_anchor(A), te_cursor(C), Max is max(A, C).
delete_selection :- ( has_sel -> sel_min(Mn), sel_max(Mx), te_apply_replace(Mn, Mx, []) ; true ).

% Byte range of the current line including its trailing newline (if any).
current_line_span(Start, End) :-
    te_cursor(C), te_line_start(C, Ls), te_line_end(C, Le), te_buffer_len(Len),
    Start = Ls,
    ( Le < Len -> End is Le + 1 ; End = Le ).

newline :- te_insert("\n").
indent :- te_tab(Tab), te_insert(Tab).

% Cursor lands at the start of the new line (te_apply_replace's default: end
% of what it just inserted, i.e. right after the newline).
open_line_below :- te_cursor(C), te_line_end(C, Pos), te_apply_replace(Pos, Pos, "\n").
% Cursor moves onto the fresh blank line above, not after the newline.
open_line_above :-
    te_cursor(C), te_line_start(C, Ls),
    te_apply_replace(Ls, Ls, "\n"),
    te_set_cursor(Ls).

delete_back :-
    ( has_sel -> delete_selection
    ; te_cursor(C),
      ( C =:= 0 -> true ; te_step_left(C, S), te_apply_replace(S, C, []) )
    ).
delete_forward :-
    ( has_sel -> delete_selection
    ; te_cursor(C), te_buffer_len(Len),
      ( C >= Len -> true ; te_step_right(C, E), te_apply_replace(C, E, []) )
    ).

copy_selection :- ( has_sel -> sel_min(Mn), sel_max(Mx), te_copy_range(Mn, Mx) ; true ).
cut_selection :- copy_selection, delete_selection.
paste_clipboard :- te_clipboard_get(Codes), ( Codes == [] -> true ; te_insert(Codes) ).

% Shift the current line one space left (outdent, only if it starts with a
% space/tab -- 32/9) or right (indent, unconditional). Both preserve the
% cursor's position *relative to the line*, which isn't where
% te_apply_replace's own default (end of the splice) would leave it, so both
% capture the pre-edit cursor and restore a shifted version afterward.
move_line_left :-
    te_cursor(C), te_line_start(C, Ls),
    ( te_byte_at(Ls, B), (B =:= 32 ; B =:= 9)
    -> End is Ls + 1,
       te_apply_replace(Ls, End, []),
       ( C > Ls -> New is C - 1 ; New = Ls ),
       te_set_cursor(New)
    ;  true
    ).
move_line_right :-
    te_cursor(C), te_line_start(C, Ls),
    te_apply_replace(Ls, Ls, " "),
    New is C + 1,
    te_set_cursor(New).

% Swap the current line with the one below/above. The cursor rides along,
% keeping its column (clamped to the shorter of the two lines).
swap_line_down :-
    te_cursor(Cursor), te_line_start(Cursor, Ls), te_line_end(Cursor, Le), te_buffer_len(Len),
    Col is Cursor - Ls,
    ( Le >= Len -> true % last line: nothing below
    ; Ns is Le + 1, te_line_end(Ns, Ne),
      A is Le - Ls, % current line length
      B is Ne - Ns, % next line length
      te_buffer_range(Ls, Le, CurLine), te_buffer_range(Ns, Ne, NextLine),
      append(NextLine, "\n", T1), append(T1, CurLine, Combined),
      te_apply_replace(Ls, Ne, Combined),
      ( Col < A -> Rel = Col ; Rel = A ),
      New is Ls + B + 1 + Rel,
      te_set_cursor(New)
    ).
swap_line_up :-
    te_cursor(Cursor), te_line_start(Cursor, Ls), te_line_end(Cursor, Le),
    Col is Cursor - Ls,
    ( Ls =:= 0 -> true % first line: nothing above
    ; PrevEnd is Ls - 1, te_line_start(PrevEnd, Ps),
      A is PrevEnd - Ps, % previous line length
      B is Le - Ls,      % current line length
      te_buffer_range(Ls, Le, CurLine), te_buffer_range(Ps, PrevEnd, PrevLine),
      append(CurLine, "\n", T1), append(T1, PrevLine, Combined),
      te_apply_replace(Ps, Le, Combined),
      ( Col < B -> Rel = Col ; Rel = B ),
      New is Ps + Rel,
      te_set_cursor(New)
    ).
move_line_up :- swap_line_up.
move_line_down :- swap_line_down.

cut_line :- current_line_span(S, E), te_copy_range(S, E), te_apply_replace(S, E, []).
copy_line :- current_line_span(S, E), te_copy_range(S, E).
% Paste the clipboard as whole line(s) above the current line -- one
% te_apply_replace call (one undo step) whether or not the clipboard already
% ends in a newline.
paste_line :-
    te_clipboard_get(Codes),
    ( Codes == [] -> true
    ; te_cursor(C), te_line_start(C, Ls),
      ( last(Codes, 10) -> Full = Codes ; append(Codes, "\n", Full) ),
      te_apply_replace(Ls, Ls, Full)
    ).
% te_set_selection (unlike te_set_cursor) already resets the blink clock, so
% there's no separate "note activity" call needed here.
select_line :- current_line_span(S, E), te_set_selection(S, E).
