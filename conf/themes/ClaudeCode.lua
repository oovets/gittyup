--
-- Claude Code theme for Gittyup
-- Colors extracted from Claude Desktop app CSS (data-theme=claude, data-mode=dark)
-- Accent brand (clay): #D87756 / hsl(14.8, 63.1%, 59.6%)
--

-- generic colors used to render borders and separators
theme['palette']   = {
  light            = '#171716',  -- gray-840 area
  midlight         = '#1F1F1D',  -- gray-800 border
  middark          = '#2F2F2D',  -- gray-750 bg-000
  dark             = '#373733',  -- gray-700

  shadow           = '#1F1E1D'   -- border color
}

-- the colors of text entry, list view, and other widgets
theme['widget']    = {
  text             = { default = '#C1BFB5', disabled = '#51504C' },  -- text-200, gray-600
  bright_text      = '#9B9991',  -- text-400
  background       = '#252523',  -- bg-100
  alternate        = '#2C2C2A',  -- gray-750
  highlight        = { active = '#D87756', inactive = '#C6613F' },  -- accent, accent-emphasized
  highlighted_text = { active = '#C1BFB5', inactive = '#9B9991' },  -- text-200, text-400
}

-- window colors
theme['window']    = {
  text             = '#B8B7B6',  -- text-300
  background       = '#2F2F2D'   -- bg-000
}

-- button colors
theme['button']    = {
  text             = { default = '#B8B7B6', inactive = '#5E5D59', disabled = '#51504C' },  -- text-300, gray-550, gray-600
  background       = { default = '#2F2F2D', checked = '#D87756', pressed = '#C6613F' }  -- bg-000, accent, emphasized
}

-- commit list colors
theme['commits']   = {
  text             = '#9B9991',  -- text-400
  bright_text      = '#C1BFB5',  -- text-200
  background       = '#2F2F2D',  -- bg-000
  alternate        = '#2F2F2D',  -- bg-000
  highlight        = { active = '#D87756', inactive = '#C6613F' },
  highlighted_text = { active = '#C1BFB5', inactive = '#9B9991' },
  highlighted_bright_text = { active = '#E1E0D9', inactive = '#9B9991' }
}

-- status badge colors
theme['badge']     = {
  foreground       = {
    normal         = '#C1BFB5',  -- text-200
    selected       = '#D87756'   -- accent
  },
  background       = {
    normal         = '#D87756',  -- accent
    selected       = '#C1BFB5',  -- text-200
    conflicted     = '#D946A8',
    head           = '#52A56A',
    notification   = '#E25558',
    modified       = '#E0AF68',
    added          = '#2D4A35',
    deleted        = '#4A2D30',
    untracked      = '#264A44',
    renamed        = '#264058'
  }
}

-- blame margin heatmap background colors
theme['blame'] = {
  cold             = '#252523',  -- bg-100
  hot              = '#4A2D30'
}

-- graph edge colors
theme['graph']     = {
  edge1            = '#D87756',
  edge2            = '#73D677',
  edge3            = '#D946A8',
  edge4            = '#E0AF68',
  edge5            = '#56B6C2',
  edge6            = '#F27983',
  edge7            = '#7AA2F7',
  edge8            = '#4ADE80',
  edge9            = '#B48EAD',
  edge10           = '#89DDFF',
  edge11           = '#ECBE7B',
  edge12           = '#98C379',
  edge13           = '#56D4BC',
  edge14           = '#7DCFFF',
  edge15           = '#FF9E64'
}

-- checkbox colors
theme['checkbox']  = {
  text             = '#B8B7B6',  -- text-300
  fill             = '#434240',  -- gray-650
  outline          = '#373733'   -- gray-700
}

-- commit editor colors
theme['commiteditor'] = {
  spellerror       = '#C04848',
  spellignore      = '#B8B7B6',  -- text-300
  lengthwarning    = '#3D3520'
}

-- diff view colors
theme['diff']      = {
  addition         = '#2D4A35',
  deletion         = '#4A2D30',
  plus             = '#60B862',
  minus            = '#C04848',
  ours             = '#1A2040',
  theirs           = '#401A40',
  word_addition    = '#1F6B30',
  word_deletion    = '#6B1F28',
  note             = '#B8B7B6',  -- text-300
  warning          = '#C49A5C',
  error            = '#6B3A3C'
}

-- link colors
theme['link']      = {
  link             = '#6A8ED6',
  link_visited     = '#9C7E97'
}

-- menubar background color
theme['menubar']   = {
  text             = '#B8B7B6',  -- text-300
  background       = '#171716'   -- darkest
}

-- tabbar background color
theme['tabbar']   = {
  text             = '#9B9991',  -- text-400
  base             = '#171716',  -- darkest
  selected         = '#2F2F2D',  -- bg-000
}

-- remote comment colors
theme['comment']   = {
  background       = '#252523',  -- bg-100
  body             = '#9B9991',  -- text-400
  author           = '#D87756',  -- accent
  timestamp        = '#7B7A79'   -- text-500
}

-- star fill color
theme['star']      = {
  fill             = '#D87756'   -- accent
}

-- titlebar background color (macOS)
theme['titlebar']  = {
  background       = '#171716'   -- darkest
}

-- popup tooltip colors
theme['tooltip']   = {
  text             = '#E1E0D9',  -- gray-100
  background       = '#D87756'   -- accent
}

-- editor styles
theme.property['color.red']          = '#C04848'
theme.property['color.yellow']       = '#C49A5C'
theme.property['color.green']        = '#60B862'
theme.property['color.teal']         = '#4A9EA8'
theme.property['color.purple']       = '#9C7E97'
theme.property['color.orange']       = '#D98A58'
theme.property['color.blue']         = '#6A8ED6'
theme.property['color.black']        = '#171716'  -- darkest
theme.property['color.grey']         = '#6E6C67'  -- gray-500
theme.property['color.white']        = '#B8B7B6'  -- text-300

-- styles
theme.property['style.bracebad']     = 'fore:#C04848'
theme.property['style.bracelight']   = 'fore:#6A8ED6'
theme.property['style.calltip']      = 'fore:#9B9991,back:#2F2F2D'        -- text-400, bg-000
theme.property['style.class']        = 'fore:#C49A5C'
theme.property['style.comment']      = 'fore:#5E5D59'                     -- gray-550 (subtle)
theme.property['style.constant']     = 'fore:#D98A58'
theme.property['style.controlchar']  = '$(style.nothing)'
theme.property['style.default']      = 'fore:#B8B7B6,back:#252523'        -- text-300, bg-100
theme.property['style.definition']   = 'fore:#6A8ED6'
theme.property['style.embedded']     = '$(style.tag),back:#2F2F2D'        -- bg-000
theme.property['style.error']        = 'fore:#C04848'
theme.property['style.function']     = 'fore:#6A8ED6'
theme.property['style.identifier']   = '$(style.nothing)'
theme.property['style.indentguide']  = 'fore:#373733,back:#373733'        -- gray-700
theme.property['style.keyword']      = 'fore:#C06A4C,bold'                -- accent dimmed
theme.property['style.label']        = 'fore:#C49A5C'
theme.property['style.linenumber']   = 'fore:#51504C,back:#171716,bold'   -- gray-600, darkest
theme.property['style.nothing']      = ''
theme.property['style.number']       = 'fore:#D98A58'
theme.property['style.operator']     = 'fore:#9B9991,bold'                -- text-400
theme.property['style.preprocessor'] = 'fore:#9C7E97,bold'
theme.property['style.regex']        = 'fore:#60B862'
theme.property['style.string']       = 'fore:#60B862'
theme.property['style.tag']          = 'fore:#C06A4C'                     -- accent dimmed
theme.property['style.type']         = 'fore:#4A9EA8'
theme.property['style.variable']     = 'fore:#6A8ED6'
theme.property['style.whitespace']   = '$(style.nothing)'
