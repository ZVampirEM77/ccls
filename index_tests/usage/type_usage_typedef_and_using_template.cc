template<typename T>
struct Foo;

using Foo1 = Foo<int>;
typedef Foo<Foo1> Foo2;

/*
OUTPUT:
{
  "includes": [],
  "skipped_by_preprocessor": [],
  "usr2func": [],
  "usr2type": [{
      "usr": 1544499294580512394,
      "detailed_name": "Foo1",
      "qual_name_offset": 0,
      "short_name": "Foo1",
      "kind": 252,
      "hover": "using Foo1 = Foo<int>",
      "declarations": [],
      "spell": "4:7-4:11|0|1|2",
      "extent": "4:1-4:22|0|1|0",
      "alias_of": 10528472276654770367,
      "bases": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [],
      "instances": [],
      "uses": ["4:7-4:11|0|1|4", "5:13-5:17|0|1|4"]
    }, {
      "usr": 10528472276654770367,
      "detailed_name": "Foo",
      "qual_name_offset": 0,
      "short_name": "Foo",
      "kind": 5,
      "declarations": ["2:8-2:11|0|1|1"],
      "alias_of": 0,
      "bases": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [],
      "instances": [],
      "uses": ["4:14-4:17|0|1|4", "5:9-5:12|0|1|4"]
    }, {
      "usr": 15933698173231330933,
      "detailed_name": "Foo2",
      "qual_name_offset": 0,
      "short_name": "Foo2",
      "kind": 252,
      "hover": "typedef Foo<Foo1> Foo2",
      "declarations": [],
      "spell": "5:19-5:23|0|1|2",
      "extent": "5:1-5:23|0|1|0",
      "alias_of": 10528472276654770367,
      "bases": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [],
      "instances": [],
      "uses": ["5:19-5:23|0|1|4"]
    }],
  "usr2var": []
}
*/
