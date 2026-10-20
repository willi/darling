# plist2lines.awk: an XML property list as one line per value, "PATH<TAB>VALUE" (PATH: keys joined by ".", array
# items as [N]; true, false, integers and strings as written), sorted by the caller
{ doc = doc $0 "\n" }
function unesc(s) {
	gsub(/&lt;/, "<", s); gsub(/&gt;/, ">", s); gsub(/&quot;/, "\"", s); gsub(/&apos;/, "'", s); gsub(/&amp;/, "\\&", s)
	return s
}
function emit(v) {
	if (inarr[depth]) { print path[depth] "[" idx[depth]++ "]\t" v } else if (key != "") { print path[depth] (path[depth] == "" ? "" : ".") key "\t" v }
	key = ""
}
function open_(arr,   p) {
	p = inarr[depth] ? path[depth] "[" idx[depth]++ "]" : (key == "" ? path[depth] : path[depth] (path[depth] == "" ? "" : ".") key)
	depth++; path[depth] = p; inarr[depth] = arr; idx[depth] = 0; key = ""
}
END {
	depth = 0; path[0] = ""; inarr[0] = 0; key = ""; started = 0
	while (match(doc, /<(key|string|integer|real|date)>[^<]*<\/(key|string|integer|real|date)>|<string\/>|<true\/>|<false\/>|<dict>|<\/dict>|<dict\/>|<array>|<\/array>|<array\/>/)) {
		t = substr(doc, RSTART, RLENGTH); doc = substr(doc, RSTART + RLENGTH)
		if (t == "<dict>") { if (started) open_(0); else started = 1 }
		else if (t == "</dict>" || t == "</array>") depth--
		else if (t == "<array>") open_(1)
		else if (t == "<dict/>" || t == "<array/>") emit("(empty)")
		else if (t == "<true/>") emit("true")
		else if (t == "<false/>") emit("false")
		else if (t == "<string/>") emit("")
		else { v = t; sub(/^<[a-z]+>/, "", v); sub(/<\/[a-z]+>$/, "", v); v = unesc(v); if (t ~ /^<key>/) key = v; else emit(v) }
	}
}
