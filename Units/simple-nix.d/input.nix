{ stdenv, fetchgit, lib }:

let
  inc = x: x + 1;
  multilineFunc =
    a:
    b:
    c:
    a + b + c;
  attrset = {
    foo = "bar";
  };
  multiline_attrset =
    { bar = "foo"; };
in {
  hello = stdenv.mkDerivation {
    name = "hello";
    src = fetchgit {
      url = "https://example.com";
      hash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    };
  };
  goodbye = buildPythonPackge {
    pname = "goodbye";
    src = fetchgit {
      url = "https://example.com";
      hash = lib.fakeSha256;
    };
  };
}
