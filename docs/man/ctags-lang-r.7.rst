.. _ctags-lang-r(7):

==============================================================
ctags-lang-r
==============================================================
-------------------------------------------------------------------
Random notes about tagging R source code with Universal Ctags
-------------------------------------------------------------------
:Version: 5.9.0
:Manual group: Universal Ctags
:Manual section: 7

SYNOPSIS
--------
|	**ctags** ... --languages=+R ...
|	**ctags** ... --language-force=R ...
|	**ctags** ... --map-Python=+.r ...

DESCRIPTION
-----------
This man page gathers random notes about tagging R source code
with Universal Ctags.

Kinds
-----------
If a variable gets a value returned from a *well-known constructor*
and the variable appears for the first time in the current input file,
the R parser makes a tag for the variable and attaches a kind
associated with the constructor to the tag regardless of whether
the variable appears in the top-level context or a function.

Well-known constructor and kind mapping

	=========== ==================
	Constructor kind
	=========== ==================
	function()  function
	=========== ==================

If a variable doesn't get a value returned from well-known
constructor, the R parser attaches "globalVar" or "functionVar" kind
to the tag for the variable depends on the context.

Here is an example demonstrating the usage of the kinds:

"input.r"

.. code-block:: R

	G <- 1
	f <- function(a) {
		g <- function (b) a + b
		L <- 2
	}

"output.tags"
with "--options=NONE --sort=no --fields=+KZ -o - input.r"

.. code-block:: tags

	G	input.r	/^G <- 1$/;"	globalVar
	f	input.r	/^f <- function(a) {$/;"	function
	g	input.r	/^	g <- function (b) a + b$/;"	function	scope:function:f
	L	input.r	/^	L <- 2$/;"	functionVar	scope:function:f

SEE ALSO
--------
:ref:`ctags(1) <ctags(1)>`
