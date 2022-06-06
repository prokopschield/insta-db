{
	"targets": [
		{
			"target_name": "instant_db_internals",
			"sources": [
				"src/db.cc"
			],
			"libraries": [
				"./blake3-build/libblake3.a",
				"./libdeflate/libdeflate.a"
			]
		}
	]
}
