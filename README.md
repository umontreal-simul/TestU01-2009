# TestU01-2009
This is the 2009 version of TestU01, a software library, implemented in the ANSI C language, and offering a collection of utilities for the empirical statistical testing of uniform random number generators. This 32-bit version is still the current official version, although a 64-bit version with many improvemnts is currently under development and may become available in late 2020.

The library implements several types of random number generators in generic form, as well as many specific generators proposed in the literature or found in widely-used software. It provides general implementations of the classical statistical tests for random number generators, as well as several others proposed in the literature, and some original ones. These tests can be applied to the generators predefined in the library and to user-defined generators. Specific tests suites for either sequences of uniform random numbers in [0,1] or bit sequences are also available. Basic tools for plotting vectors of points produced by generators are provided as well.

Additional software permits one to perform systematic studies of the interaction between a specific test and the structure of the point sets produced by a given family of random number generators. That is, for a given kind of test and a given class of random number generators, to determine how large should be the sample size of the test, as a function of the generator's period length, before the generator starts to fail the test systematically.

The documentation with a description of the functions in TestU01 is available in the user's guide below.

## TestU01-1.2.3
This version was created on 18 August 2009.

- Licence and [copyright](http://simul.iro.umontreal.ca/testu01/copyright.html).
- The installation uses configure.
- [Binaries for Cygwin under MS Windows](http://simul.iro.umontreal.ca/testu01/bin-cygwin.zip)
- [Binaries for MinGW under MS Windows](http://simul.iro.umontreal.ca/testu01/bin-mingw.zip)
- [Installation](http://simul.iro.umontreal.ca/testu01/install.html)
- [User's guide](http://simul.iro.umontreal.ca/testu01/guideshorttestu01.pdf) (pdf)
- [Paper](http://portal.acm.org/citation.cfm?doid=1268776.1268777) (pdf) describing TestU01 with results from our test suites applied on several popular generators: P. L'Ecuyer and R. Simard, TestU01: A C Library for Empirical Testing of Random Number Generators ACM Transactions on Mathematical Software, Vol. 33, article 22, 2007.
      ERRATUM: The period of generator Brent-xor4096s in Table I should be 2^4128 and not 2^131072.
      
## Contact us
To submit a bug or a comment, send an e-mail to:
[simul@iro.umontreal.ca](simul@iro.umontreal.ca)
or create a pull request.
