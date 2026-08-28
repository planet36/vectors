# COMMENT-STYLE.md

The rules for prose in this repo.  They cover code comments, Doxygen blocks,
and commit messages.  Documentation files follow them too, though a table or a
reference list may need its own shape.

These rules exist because one habit produced almost every prose complaint
raised against this codebase.  That habit is packing several ideas into one
sentence and welding them together with punctuation instead of writing several
sentences.  The cure is not a ban on complex sentences.  Clauses that belong to
one thought are better joined than chopped apart, and the rules below are about
where that line falls.

Write the rules into the first draft.  A comment that needs a rewrite pass to
satisfy them was written wrong.

## Sentences

**1. One thought per sentence.**  A sentence may carry as many clauses as that
thought needs, and joining related clauses usually reads better than splitting
them into short, halting sentences.  The fault this rule names is a sentence
carrying two thoughts that the reader has to pull apart.  Split that one.

**2. Semicolons are rare.**  A semicolon joins two independent clauses, so ask
first whether a comma and a conjunction would carry them, and whether the
second clause wants to be its own sentence.  Keep the semicolon when neither of
those reads as well.

**3. Colons introduce lists and explanations.**  A colon before a real list is
correct, and so is one whose second half explains the clause before it.  What
sits before the colon should be a clause rather than a bare noun phrase used as
a label.

**4. Dashes set off a parenthetical or land a closing clause.**  A pair of
dashes around a parenthetical is acceptable, and real parentheses are usually
better.  A single dash may set off a trailing clause where the pause earns its
keep, but a fragment that wanted to be a sentence is not fixed by hanging it
off a dash.  Spell a dash as two hyphens.  Do not use a real em dash.

**5. Finish the sentence.**  Detail paragraphs are complete sentences with
verbs.  A `///` brief line may be a noun phrase, and it stays on one line.

**5a. A brief takes no terminating period.**  That holds whether the brief
reads as a noun phrase or as a full sentence, so every brief in a file ends the
same way.  A `///<` trailing brief follows the same rule.  Detail paragraphs
inside the block punctuate normally.

## Paragraphs

**6. Keep a paragraph to about five lines.**  Past that, split it into two
paragraphs or cut it down.  A wall of text is not thorough, it is unread.

**7. Put two spaces after a sentence-ending period** in code comments and
Doxygen blocks.  A markdown file follows whatever that file already does.
This file uses two spaces.  `README.md`, `DESIGN.md`, and `CLAUDE.md` use one.
Match the file you are editing rather than converting it.

**7a. Keep a line to 96 columns.**  That is a ceiling for code and prose alike.
Comment prose is wrapped narrower than that, and a paragraph you edit keeps the
width it already had rather than being reflowed to the ceiling.  A markdown file
that puts each paragraph on one long line stays that way.

**7b. Put one space before a trailing comment.**  That is the gap between code
and the `//` or `///<` that follows it on the same line.  Rule 7 governs a
different place, so do not let the two spaces after a period pull this one to
two.

A run of trailing comments on consecutive declarations is the one exception.
It may pad out to a shared column instead, and then every comment in the run
lines up on that column.  A lone trailing comment is not a run, and neither is
one that follows a statement.

## Content

**8. Do not restate the declaration.**  The reader can see the types, the
parameter names, and the return type.  A comment earns its place by saying
something the code does not.

**9. Do not explain what was rejected.**  Rationale for a road not taken
belongs in a plan document or a ledger, never in a comment or a commit message.

**10. Do not state the obvious.**  "Owned by a `unique_ptr`" next to a
`unique_ptr` member is noise.

**10a. Write an argument once.**  A second site that needs it states the
consequence in a sentence and points at where the argument lives.  Copying the
reasoning means every copy is a place to update when the answer changes.  Point
only at something that travels with the file, so that a header copied into
another program does not end up referring to a document it left behind.

A header must stand alone.  These headers are destined for another repository,
which will have neither `README.md` nor `CLAUDE.md`, so a header may point only
at a sibling header.  Where the argument lives in a file that does not travel,
the consequence sentence carries no pointer and stands by itself.  Such a
sentence is the only copy that survives the move, so never thin one into a
pointer, and never let a gotcha live only in a file the headers leave behind.

One case earns a second copy.  An argument written to head off a suggestion has
to sit where the reader forms the suggestion, because a reader who never follows
the pointer is exactly the one it was written for.  Such a copy carries the
argument alone.  Do not append a line saying the real rationale lives
elsewhere.

## Doxygen

**10b. The brief goes on a `///` line above the block.**  Write the one-line
brief as `///` immediately above the `/** ... */` block, and let the block hold
only the detail.  Do not use `\brief` inside the block.  A declaration whose
brief says everything needs no block at all.

**10c. Refer to a parameter with `\a`, not `\p`.**  Both mark a word as a
parameter in running text.  `\a` renders it italic and `\p` renders it
monospace.  The italic sets a parameter apart, where monospace blends it into
the identifiers the prose already writes that way.  Neither one checks that
the word names a real parameter, so a typo in either is silent.  Only
`\param` checks.

**10d. Line the block's stars up with its `/**`.**  A continuation line inside
a `/** ... */` block starts its `*` in the same column as the opening `/**`,
not one column to the right.  The closing `*/` sits in that column too.  A
block indented inside a class indents its stars to match its own `/**`.

**10e. Document an exception with `\exception`, never `\throw` or `\throws`.**
Doxygen renders all three identically, which was checked against version
1.18.0.  Committing to one spelling is what keeps a search for the tag
exhaustive.  `\exception` is the noun, which matches the `\param` and
`\return` it sits beside.

**11. Use `\copydoc` when the whole block transfers.**  It copies the `\param`
list along with the text, so it only works when the parameter names match.

**12. Use `\copybrief` plus one specific paragraph when they do not.**  Two
overloads that spell the same parameter differently are the usual case.  One
taking `data` and `len` cannot `\copydoc` from one taking `src`, because the
imported `\param` names would not match the signature.

**13. Verify that a reference resolves.**  Doxygen cannot tell two overloads
apart by a `requires` clause alone.  A reference that silently resolves to the
wrong one is worse than no reference.

## Wording

**14. American spellings.**  Watch `-our`, `-ise`, `-re`, and "judgment".
Never "programme", in any sense.

**15. Use the serial comma** before the final conjunction.

**16. Never write "load-bearing".**  Name the actual dependency instead.
