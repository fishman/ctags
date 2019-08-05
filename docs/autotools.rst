Building with configure (\*nix including GNU/Linux)
---------------------------------------------------------------------

To install Universal-ctags' dependencies on Debian-based systems, do::

	$ sudo apt install \
		  pkg-config autoconf python3-docutils \
		  libseccomp-dev libjansson-dev

As with most Autotools-based projects, you'll need to do::

    $ ./autogen.sh
    $ ./configure --prefix=/where/you/want # defaults to /usr/local
    $ make
    $ make install # may require extra privileges depending on where to install

After installing, the `ctags` executable can be found in `$prefix/bin/`.

`autogen.sh` runs `autoreconf` internally.
If you use a (binary oriented) GNU/Linux distribution, `autoreconf` may
be part of the `autoconf` package. In addition you may have to install
`automake` and/or `pkg-config`, too.

Changing the executable's name
,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,

On some systems, like certain BSDs, there is already a 'ctags' program in the base
system, so it is somewhat inconvenient to have the same name for
Universal-ctags. During the ``configure`` stage you can now change
the name of the created executable.

To add a prefix 'ex' which will result in 'ctags' being renamed to 'exctags':

.. code-block:: bash

	$ ./configure --program-prefix=ex

To completely change the program's name run the following:

.. code-block:: bash

	$ ./configure --program-transform-name='s/ctags/my_ctags/; s/etags/myemacs_tags/'

Please remember there is also an 'etags' installed alongside 'ctags' which you may also want to rename as shown above.
