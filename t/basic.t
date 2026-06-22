use Test::Nginx::Socket 'no_plan';
run_tests();

__DATA__

=== TEST 1: HTML whitespace + comment stripped
--- config
    strip on;
    return 200 "<html>\n  <body>\n    <!-- comment -->\n    <p>Hello   world</p>\n  </body>\n</html>";
    default_type text/html;
--- request
GET /
--- response_body: <html><body><p>Hello world</p></body></html>
--- no_error_log
[error]

=== TEST 2: HTML <pre> body preserved verbatim
--- config
    strip on;
    return 200 "<div>\n<pre>keep\n  all   spaces\n</pre>\n</div>\n";
    default_type text/html;
--- request
GET /
--- response_body
<div><pre>keep
  all   spaces
</pre></div>
--- no_error_log
[error]

=== TEST 3: HTML <textarea> body preserved
--- config
    strip on;
    return 200 "<form>\n<textarea>\nhello   world\n</textarea>\n</form>\n";
    default_type text/html;
--- request
GET /
--- response_body
<form><textarea>
hello   world
</textarea></form>
--- no_error_log
[error]

=== TEST 4: HTML <script> body preserved verbatim
--- config
    strip on;
    return 200 "<html>\n<script>\nvar x = 1;\n// comment\n</script>\n</html>\n";
    default_type text/html;
--- request
GET /
--- response_body
<html><script>
var x = 1;
// comment
</script></html>
--- no_error_log
[error]

=== TEST 5: HTML <style> body preserved verbatim
--- config
    strip on;
    return 200 "<html>\n<style>\nbody {\n  color: red;\n}\n</style>\n</html>\n";
    default_type text/html;
--- request
GET /
--- response_body
<html><style>
body {
  color: red;
}
</style></html>
--- no_error_log
[error]

=== TEST 6: JSON minified, in-string whitespace preserved
--- config
    strip_json on;
    return 200 '{"a": 1, "b": "two  spaces", "c": [1, 2]}';
    default_type application/json;
--- request
GET /
--- response_body: {"a":1,"b":"two  spaces","c":[1,2]}
--- no_error_log
[error]

=== TEST 7: CSS stripped, block comment removed
--- config
    strip_css on;
    return 200 "body {\n  color: red; /* x */\n  margin:  0;\n}\n";
    default_type text/css;
--- request
GET /
--- response_body: body{color:red;margin:0;}
--- no_error_log
[error]

=== TEST 8: JS line + block comments stripped, ASI newline preserved
--- config
    strip_js on;
    return 200 "function f() {\n  var a = 1 // line\n  return a /* blk */ + 2\n}\n";
    default_type application/javascript;
--- request
GET /
--- response_body
function f(){var a=1
return a+2}
--- no_error_log
[error]

=== TEST 9: JS string literal preserved (no stripping inside)
--- config
    strip_js on;
    return 200 "var s = 'a  b\\nc';\nvar t = 1;\n";
    default_type application/javascript;
--- request
GET /
--- response_body: var s='a  b\nc';var t=1;
--- no_error_log
[error]

=== TEST 10: JS regex literal preserved
--- config
    strip_js on;
    return 200 "var re = /a\\/b/g\nvar y = 10 / 2\n";
    default_type application/javascript;
--- request
GET /
--- response_body
var re=/a\/b/g
var y=10/2
--- no_error_log
[error]

=== TEST 11: strip off by default — no transform
--- config
    return 200 "<html>\n  <!-- x -->\n  <p>hello</p>\n</html>\n";
    default_type text/html;
--- request
GET /
--- response_body
<html>
  <!-- x -->
  <p>hello</p>
</html>
--- no_error_log
[error]

=== TEST 12: strip_json off — JSON not minified
--- config
    return 200 '{"a": 1}';
    default_type application/json;
--- request
GET /
--- response_body: {"a": 1}
--- no_error_log
[error]

=== TEST 13: strip on but wrong content-type — no transform
--- config
    strip on;
    return 200 '{"a": 1}';
    default_type application/json;
--- request
GET /
--- response_body: {"a": 1}
--- no_error_log
[error]

=== TEST 14: Content-Length cleared, weak ETag on stripped response
--- config
    strip on;
    return 200 "<html>\n  <p>hi</p>\n</html>";
    default_type text/html;
--- request
GET /
--- response_headers
! Content-Length
--- no_error_log
[error]

=== TEST 15: strip_min_size skips small bodies
--- config
    strip on;
    strip_min_size 9999;
    return 200 "<html>\n  <!-- x -->\n  <p>hello</p>\n</html>\n";
    default_type text/html;
--- request
GET /
--- response_body
<html>
  <!-- x -->
  <p>hello</p>
</html>
--- no_error_log
[error]

=== TEST 16: text/javascript also stripped by strip_js
--- config
    strip_js on;
    return 200 "var x = 1; // comment\nvar y = 2;\n";
    default_type text/javascript;
--- request
GET /
--- response_body: var x=1;var y=2;
--- no_error_log
[error]

=== TEST 17: JSON nested object + array
--- config
    strip_json on;
    return 200 '{ "x": { "y": [ 1, 2, 3 ] } }';
    default_type application/json;
--- request
GET /
--- response_body: {"x":{"y":[1,2,3]}}
--- no_error_log
[error]

=== TEST 18: HTML multiple adjacent whitespace runs between words
--- config
    strip on;
    return 200 "<p>one   two\n\nthree</p>";
    default_type text/html;
--- request
GET /
--- response_body: <p>one two three</p>
--- no_error_log
[error]

=== TEST 19: CSS string literals preserved
--- config
    strip_css on;
    return 200 "a::after { content: '  hi  '; }\n";
    default_type text/css;
--- request
GET /
--- response_body: a::after{content:'  hi  ';}
--- no_error_log
[error]

=== TEST 20: Empty body passes through without error
--- config
    strip on;
    return 204;
    default_type text/html;
--- request
GET /
--- error_code: 204
--- no_error_log
[error]
