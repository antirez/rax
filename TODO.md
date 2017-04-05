ASAP
===

* Check failed lookup test speed regression after a given commit.
* Test the insertion of strings greater then 512 MB. Add unit test for empty.
  string set/get and iteration.
* Avoid fixing the parent link if the node is the same, if this makes a speed difference because of the avoided cache miss.
* Check if reclaiming nodes from first to last child is a performance improvement in `radtreeFree()`.
* Explicit unit test with `NULL` values.
* Explocit unit test with empty string.
* Finish `README`.
* Turn repository into public.

Potential features to add in the future
===

* Ability to store aux data of any size inside the node itself, instead of the
  pointer. Could be done setting a special first byte in the value section
  to represent an encoded and progressive data length. This could further
  reduce the memory needs of a radix tree to represent a string `->` string map.
* Rank operations. Only if opt-in during the Rax creation, otherwise no memory cost should be payed. If this feature gets added, to extract a random element we can just scan the radix tree for the N-th element after extracting a number N at random between zero and the number of elements minus one.
