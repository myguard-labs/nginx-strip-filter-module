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
--- response_body
body{color:red;margin:0}
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
--- response_body
var s='a  b\nc';var t=1;
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
--- response_body
var x=1;var y=2;
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
--- response_body
a::after{content:'  hi  '}
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

=== TEST 21: CSS zero-unit stripping (px, em, %)
--- config
    strip_css on;
    return 200 "div { margin: 0px; padding: 0em; top: 0%; font-size: 10px; }\n";
    default_type text/css;
--- request
GET /
--- response_body
div{margin:0;padding:0;top:0;font-size:10px}
--- no_error_log
[error]

=== TEST 22: CSS #rrggbb collapsed to #rgb
--- config
    strip_css on;
    return 200 "a { color: #ffaabb; background: #aabbcc; border-color: #112233; }\n";
    default_type text/css;
--- request
GET /
--- response_body
a{color:#fab;background:#abc;border-color:#123}
--- no_error_log
[error]

=== TEST 23: CSS #rrggbb not collapsed when pairs differ
--- config
    strip_css on;
    return 200 "a { color: #ff00ab; }\n";
    default_type text/css;
--- request
GET /
--- response_body
a{color:#ff00ab}
--- no_error_log
[error]

=== TEST 24: HTML boolean attribute collapsed
--- config
    strip on;
    return 200 '<input disabled="disabled" checked="checked" readonly="readonly">';
    default_type text/html;
--- request
GET /
--- response_body: <input disabled checked readonly>
--- no_error_log
[error]

=== TEST 25: HTML non-boolean attribute not collapsed (but unquoted)
--- config
    strip on;
    return 200 '<input type="text" value="hello">';
    default_type text/html;
--- request
GET /
--- response_body: <input type=text value=hello>
--- no_error_log
[error]

=== TEST 26: SVG comment stripped and whitespace collapsed
--- config
    strip_svg on;
    return 200 '<svg xmlns="http://www.w3.org/2000/svg">
  <!-- title -->
  <circle r="5" />
</svg>
';
    default_type image/svg+xml;
--- request
GET /
--- response_body
<svg xmlns="http://www.w3.org/2000/svg"><circle r="5" /></svg>
--- no_error_log
[error]

=== TEST 27: SVG CDATA preserved verbatim
--- config
    strip_svg on;
    return 200 '<svg>
  <script><![CDATA[  var x = 1;  ]]></script>
</svg>
';
    default_type image/svg+xml;
--- request
GET /
--- response_body
<svg><script><![CDATA[  var x = 1;  ]]></script></svg>
--- no_error_log
[error]

=== TEST 28: SVG off by default — no transform
--- config
    return 200 '<svg>  <!-- x -->  <g/></svg>';
    default_type image/svg+xml;
--- request
GET /
--- response_body: <svg>  <!-- x -->  <g/></svg>
--- no_error_log
[error]

=== TEST 29: CSS zero-unit rem and vw also stripped
--- config
    strip_css on;
    return 200 "p { gap: 0rem; width: 0vw; height: 0vh; }\n";
    default_type text/css;
--- request
GET /
--- response_body
p{gap:0;width:0;height:0}
--- no_error_log
[error]

=== TEST 30: CSS trailing semicolon before } dropped
--- config
    strip_css on;
    return 200 "a{color:red;}";
    default_type text/css;
--- request
GET /
--- response_body chomp: a{color:red}
--- no_error_log
[error]

=== TEST 31: CSS leading zero stripped (0.5 -> .5)
--- config
    strip_css on;
    return 200 "a{opacity:0.5;margin:0.25em 1.5px}";
    default_type text/css;
--- request
GET /
--- response_body chomp: a{opacity:.5;margin:.25em 1.5px}
--- no_error_log
[error]

=== TEST 32: CSS leading zero not stripped inside number (10.5)
--- config
    strip_css on;
    return 200 "a{width:10.5px;top:100.0px}";
    default_type text/css;
--- request
GET /
--- response_body chomp: a{width:10.5px;top:100.0px}
--- no_error_log
[error]

=== TEST 33: CSS url() unquoted content preserved verbatim
--- config
    strip_css on;
    return 200 "a{background:url(img/0.5x logo.png)}";
    default_type text/css;
--- request
GET /
--- response_body chomp: a{background:url(img/0.5x logo.png)}
--- no_error_log
[error]

=== TEST 34: CSS url() quoted content preserved verbatim
--- config
    strip_css on;
    return 200 "a{background:url( \"a b.png\" )}";
    default_type text/css;
--- request
GET /
--- response_body chomp: a{background:url( "a b.png" )}
--- no_error_log
[error]

=== TEST 35: CSS only-semicolon-before-close in nested rules
--- config
    strip_css on;
    return 200 "@media screen{a{color:red;}b{color:blue;}}";
    default_type text/css;
--- request
GET /
--- response_body chomp: @media screen{a{color:red}b{color:blue}}
--- no_error_log
[error]

=== TEST 36: XML (application/xml) comment + whitespace stripped
--- config
    strip_xml on;
    return 200 '<?xml version="1.0"?>
<root>
  <!-- c -->
  <item>x</item>
</root>
';
    default_type application/xml;
--- request
GET /
--- response_body
<?xml version="1.0"?><root><item>x</item></root>
--- no_error_log
[error]

=== TEST 37: RSS (application/rss+xml) matched via +xml suffix
--- config
    strip_xml on;
    return 200 '<rss>
  <channel>
    <title>T</title>
  </channel>
</rss>
';
    default_type application/rss+xml;
--- request
GET /
--- response_body
<rss><channel><title>T</title></channel></rss>
--- no_error_log
[error]

=== TEST 38: XML CDATA preserved verbatim
--- config
    strip_xml on;
    return 200 '<item><![CDATA[  keep   me  ]]></item>';
    default_type text/xml;
--- request
GET /
--- response_body chomp: <item><![CDATA[  keep   me  ]]></item>
--- no_error_log
[error]

=== TEST 39: strip_xml matches +xml with charset parameter
--- config
    strip_xml on;
    return 200 '<a>  <b/>  </a>';
    default_type "application/atom+xml; charset=utf-8";
--- request
GET /
--- response_body chomp: <a><b/></a>
--- no_error_log
[error]

=== TEST 40: strip_xml off by default — no transform
--- config
    return 200 '<root>  <!-- x -->  <a/></root>';
    default_type application/xml;
--- request
GET /
--- response_body: <root>  <!-- x -->  <a/></root>
--- no_error_log
[error]

=== TEST 41: strip_xml does not touch JSON content-type
--- config
    strip_xml on;
    return 200 '{ "a" : 1 }';
    default_type application/json;
--- request
GET /
--- response_body: { "a" : 1 }
--- no_error_log
[error]

=== TEST 42: HTML safe attr value unquoted
--- config
    strip on;
    return 200 '<a href="page" class="btn">x</a>';
    default_type text/html;
--- request
GET /
--- response_body chomp: <a href=page class=btn>x</a>
--- no_error_log
[error]

=== TEST 43: HTML attr value with space keeps quotes
--- config
    strip on;
    return 200 '<a class="a b">x</a>';
    default_type text/html;
--- request
GET /
--- response_body chomp: <a class="a b">x</a>
--- no_error_log
[error]

=== TEST 44: HTML empty attr value keeps quotes
--- config
    strip on;
    return 200 '<input value="">';
    default_type text/html;
--- request
GET /
--- response_body chomp: <input value="">
--- no_error_log
[error]

=== TEST 45: HTML unquote skipped before self-closing slash
--- config
    strip on;
    return 200 '<img src="a.png"/>';
    default_type text/html;
--- request
GET /
--- response_body chomp: <img src="a.png"/>
--- no_error_log
[error]

=== TEST 46: HTML attr value with > keeps quotes
--- config
    strip on;
    return 200 '<a data-x="a>b">y</a>';
    default_type text/html;
--- request
GET /
--- response_body chomp: <a data-x="a>b">y</a>
--- no_error_log
[error]

=== TEST 47: HTML adjacent attrs after quote keep quotes (no merge)
--- config
    strip on;
    return 200 '<a href="x"id="y">z</a>';
    default_type text/html;
--- request
GET /
--- response_body chomp: <a href="x"id=y>z</a>
--- no_error_log
[error]

=== TEST 48: HTML boolean collapse still works alongside unquote
--- config
    strip on;
    return 200 '<input type="text" disabled="disabled" name="q">';
    default_type text/html;
--- request
GET /
--- response_body chomp: <input type=text disabled name=q>
--- no_error_log
[error]
