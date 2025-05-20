{
	"variables": {
		"module_name": "pcsclite",
		"module_path": "./build/Release/"
	},
	"targets": [
		{
			"target_name": "<(module_name)",
			"sources": [
				"src/addon.cpp",
				"src/pcsclite.cpp",
				"src/cardreader.cpp"
			],
			"cflags": [
				"-Wall",
				"-Wextra",
				"-Wno-unused-parameter",
				"-fPIC",
				"-fno-strict-aliasing",
				"-fno-exceptions",
				"-pedantic"
			],
			"include_dirs": [
				"<!@(node -p \"require('node-addon-api').include\")"
			],
			"dependencies": [
				"<!(node -p \"require('node-addon-api').gyp\")"
			],
			"defines": [ 
				"NAPI_DISABLE_CPP_EXCEPTIONS",
				"NAPI_VERSION=8"
			],
			"conditions": [
				[
					"OS=='linux'",
					{
						"include_dirs": [
							"/usr/include/PCSC"
						],
						"link_settings": {
							"libraries": [
								"-lpcsclite"
							],
							"library_dirs": [
								"/usr/lib"
							]
						}
					}
				],
				[
					"OS=='mac'",
					{
						"libraries": [
							"-framework",
							"PCSC"
						]
					}
				],
				[
					"OS=='win'",
					{
						"libraries": [
							"-lWinSCard"
						]
					}
				]
			]
		},
		{
			"target_name": "action_after_build",
			"type": "none",
			"dependencies": ["<(module_name)"]
		}
	]
}
