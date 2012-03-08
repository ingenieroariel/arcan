-- Keyconf.lua
----------------------------------------------------------
-- includable by any theme
-- will go through a table of desired mappings,
-- and associate these with input events. 
-- These can then be serialized to a file that the various tools
-- can convert to different targets (generating 
-- configurations) for external launch,
-- or used for conversion in internal launch.
-- modify the possible entries as you see fit, these are only helpers. 
----------------------------------------------------------

-- first char is directive, 
-- whitespace : optional
-- r : required
-- a : analog

local default_menu_group = {
	"rMENU_ESCAPE",
	"rMENU_UP",
	"rMENU_DOWN",
	"rMENU_LEFT",
	"rMENU_RIGHT",
	"rMENU_SELECT",
	"aCURSOR_X",
	"aCURSOR_Y"
};

-- is iterated until the player signals escape,
-- also, the pattern PLAYERN is also inserted, where N is the
-- current number of the player 
local default_player_group = {
	"rUP",
	"rDOWN",
	"rLEFT",
	"rRIGHT",
	"rSTART",
	" SELECT",
    "rCOIN1"
};

local function keyconf_color(self, borderr, borderg, borderb, windowr, windowg, windowb)
	local bprops = {x = 0, y = 0, width = 1, height = 1, opa = 1.0};
	local wprops = {opa = 1.0, width = 1, height = 1, x = 3, y = 3};
-- copy surface properties so that we can reset them on the new objects
	if (self.window and self.window ~= BADID) then wprops = image_surface_properties(self.window); delete_image(self.window); end
	if (self.border and self.border ~= BADID) then bprops = image_surface_properties(self.border); delete_image(self.border); end

	self.border = fill_surface(8, 8, borderr, borderg, borderb);
	self.window = fill_surface(8, 8, windowr, windowg, windowb);

-- we associate with an anchor used for movement so that we can clip to
-- window rather than border
	link_image(self.border, self.anchor);
	link_image(self.window, self.anchor);
	
	image_mask_clear(self.border, MASK_SCALE);
	image_mask_clear(self.border, MASK_OPACITY);
	image_mask_clear(self.window, MASK_SCALE);
	image_mask_clear(self.window, MASK_OPACITY);

-- reset all the old properties
	resize_image(self.border, bprops.width, bprops.height);
	resize_image(self.window, wprops.width, wprops.height);
	move_image(self.window, wprops.x, wprops.y);
	move_image(self.border, bprops.x, bprops.y);
	blend_image(self.window, wprops.opa);
	blend_image(self.border, bprops.opa);
end

local function keyconf_opacity(self, borderopa, windowopa)
	blend_image(self.window, windowopa);
	blend_image(self.border, borderopa);
end

local function keyconf_tofront(self)
	self.order = max_current_image_order() + 1;
	order_image(self.window, self.order+1);
	order_image(self.border, self.order);
	if (self.textvid) then
		order_image(self.textvid, self.order+2);
	end
end

local function keyconf_destroy(self)
	expire_image(self.window, 20);
	blend_image(self.window, 0.0, 20);
	expire_image(self.border, 20);
	blend_image(self.border, 0.0, 20);

	delete_image(self.textvid);
	self.active = true;
	self:flush();
end

local function keyconf_renderline(self, string, size)
	if (self.textvid) then
		delete_image(self.textvid);
		self.textvid = nil;
	end

	if (size == nil) then
	    size = 18
	end

-- push to front, render text, resize window, align to current resolution 
	self.textvid = render_text([[\ffonts/default.ttf,]] .. size .. " " .. string);
	self.line = string;
	prop = image_surface_properties(self.textvid);

	link_image(self.textvid, self.window);
	image_mask_clear(self.textvid, MASK_OPACITY);
	image_clip_on(self.textvid);

	move_image(self.anchor, VRESW * 0.5 - prop.width * 0.5, VRESH * 0.5 - (prop.height + 10) * 0.5, 0);
	resize_image(self.window, prop.width + 10, prop.height + 10 , NOW);
	resize_image(self.border, prop.width + 16, prop.height + 16, NOW);

	move_image(self.textvid, 5, 5, NOW);
	order_image(self.textvid, self.order + 2);
	blend_image(self.textvid, 1.0);
end


local function keyconf_new(self)
	self.used = {};
	self.table = {};
	self.window_color = keyconf_color;
	self.window_opacity = keyconf_opacity;
	self.to_front = keyconf_tofront;

	self.anchor = fill_surface(1, 1, 0, 0, 0);
	self.yofs = 6;

	self:window_color(255, 255, 255, 0, 0, 164);
	self:window_opacity(0.9, 0.6);
	self:to_front();
	resize_image(self.border, VRESW / 3, VRESH * 0.2, 5);
	resize_image(self.window, VRESW / 3 - 6, VRESH * 0.2 - 6, 5);

	keyconf_renderline(self, [[\#ffffffWelcome to Arcan keyconfig!\r\nPlease press a button for MENU_ESCAPE (required)]], 18);
	self.label = [[Please press a button for MENU_ESCAPE (required)]];

	self.key = "MENU_ESCAPE";
	self.key_kind = "r";

	self.configlist = {}
	self.ofs = 1;
	ofs = 1;

	for i=1,#self.menu_group do
		self.configlist[ofs] = self.menu_group[i];
		ofs = ofs + 1;
	end
	
end


local function keyconf_playerline(self)
	line = [[Configure player input (press SELECT to continue):\n\r\t\t#Players\t#Buttons\n\r]]
	if (self.active_group == 0) then
		line = line .. [[\#ffff00]] .. tostring(self.playercount) .. [[\t\t\#ffffff]] .. tostring(self.buttoncount);
	else
		line = line .. [[\#ffffff]] .. tostring(self.playercount) .. [[\t\t\#ffff00]] .. tostring(self.buttoncount);
	end

	keyconf_renderline(self, line);
end

-- query the user for the next input table,
-- true on new key
-- false otherwise

local function keyconf_next_key(self)
	self.ofs = self.ofs + 1;

	if (self.ofs <= # self.configlist) then
 
		self.key = self.configlist[self.ofs];
		self.key_kind = string.sub(self.key, 1, 1);
		self.key = string.sub(self.key, 2);

		local lbl;
	
		lbl = "(".. tostring(self.ofs) .. " / " ..tostring(# self.configlist) ..")";

		if (self.key_kind == "A" or self.key_kind == "r") then
			lbl = "(required) ";
		else
			lbl = "(optional) ";
		end

		if (self.key_kind == "a" or self.key_kind == "A") then
			lbl = lbl .. [[ Please provide input along \ione \!iaxis on an analog device for:\n\r ]] .. self.key .. [[\t 0 samples grabbed]];
			self.analog_samples = {};
		else
			lbl = lbl .. "Please press a button for " .. self.key;
		end

		self.label = lbl;
		keyconf_renderline(self, self.label );

        return false;
    else
-- if we've already configured the player inputs, then we're done. Otherwise, remap temporarily for the "# of players" input..
		if (self.playerconf) then
			self:destroy();
			return true;
		else
			self.input = self.input_playersel;
			self.active_group = 0;
			self.playercount  = 0;
			self.buttoncount  = 0;
			keyconf_playerline(self);
			return false;
		end
    end
end

local function keyconf_inp_playersel(self, inputtable)
	local lbl = self:match(inputtable);

	if (inputtable.active and lbl) then
		for ind, val in pairs(lbl) do 
			if (val == "MENU_UP" or val == "MENU_DOWN") then
				local dir = 1;
				if (val == "MENU_DOWN") then dir = -1; end
				if (self.active_group == 0) then
					self.playercount = (self.playercount + dir) > 0 and (self.playercount + dir) or 0;
				else
					self.buttoncount = (self.buttoncount + dir) > 0 and (self.buttoncount + dir) or 0;
				end
			
			elseif (val == "MENU_LEFT" or val == "MENU_RIGHT") then
				self.active_group = self.active_group == 1 and 0 or 1;

			elseif (val == "MENU_SELECT") then
				self.input = self.defaultinput;
				self.playerconf = true;
				self.ofs = self.ofs - 1;
				
				for i=1, self.buttoncount do
					table.insert(default_player_group, "rBUTTON" .. tostring(i));
				end
				
				if (self.playercount > 0 and self.buttoncount > 0) then
					for i=1,self.playercount do
						for j=1,#self.player_group do
							kind = string.sub(self.player_group[j], 1, 1);
							table.insert(self.configlist, kind .. "PLAYER" .. i .. "_" .. string.sub(self.player_group[j], 2));
						end
					end
				end

				return keyconf_next_key(self);
			end

			keyconf_playerline(self);
			return false;
		end
	end

	return false;
end


-- return the symstr that match inputtable, or nil.
local function keyconf_match(self, input, label)
	if (input == nil) then
        return nil;
    end

    if (type(input) == "table") then
		kv = self.table[ self:id(input) ];
    else
		kv = self.table[ input ];
    end

-- with a label set, we expect a boolean result
-- otherwise the actual matchresult will be returned

	if label ~= nil and kv ~= nil then
		if type(kv) == "table" then
			for i=1, #kv do
				if kv[i] == label then return true; end
			end
		else
			return kv == label;
		end
		
		return false;
    end

    return kv;
end

local function keyconf_tbltoid(self, inputtable)
    if (inputtable.kind == "analog") then
		return "analog:" ..inputtable.devid .. ":" .. inputtable.subid .. ":" .. inputtable.source;
    end
    
    if inputtable.translated then
		if self.ignore_modifiers then
			return "translated:" .. inputtable.devid .. ":" .. inputtable.keysym;
		else
			return "translated:" .. inputtable.devid .. ":" .. inputtable.keysym .. ":" .. inputtable.modifiers;
		end
    else
		return "digital:" .. inputtable.devid .. ":" .. inputtable.subid .. ":" .. inputtable.source;
    end
end

-- associate 
local function keyconf_set(self, inputtable)

-- forward lookup: 1..n
    local id = self:id(inputtable);
	if (self.table[id] == nil) then
		self.table[id] = {};
    end
    
	table.insert(self.table[id], self.key);

	self.table[self.key] = id;
end

-- return true on more samples needed,
-- false otherwise.
local function keyconf_analog(self, inputtable)
    -- find which axis that is active, sample 'n' numbers
	table.insert(self.analog_samples, self:id(inputtable));

	self.label = "(".. tostring(self.ofs) .. " / " ..tostring(# self.configlist) ..")";
	self.label = self.label .. [[ Please provide input along \ione \!iaxis on an analog device for:\n\r ]] .. self.key .. [[\t ]] .. tostring(# self.analog_samples) .. " samples grabbed (100+ needed)";

    counttable = {}

	for i=1,#self.analog_samples do
		val = counttable[ self.analog_samples[i] ] or 0;
		counttable[ self.analog_samples[i] ] = val + 1;
    end

    max = 1;
    maxkey = "not found";

    for key, value in pairs( counttable ) do
        if (value > max) then
           max = value;
           maxkey = key;
        end
    end

	self.label = self.label .. [[\n\r dominant device(:axis) is (]] .. maxkey .. ")";
	if ( #self.analog_samples > 100 and maxkey == self:id(inputtable) ) then
		self:set(inputtable);
		return self:next_key();
    else
		keyconf_renderline(self, self.label );
		return true;
	end
end

local function keyconf_input(self, inputtable)
	if (self.active or self.key == nil) then
		return true;
	end
	
-- early out, MENU_ESCAPE is defined to skip labels where this is allowed,
-- meaning a keykind of ' ' or 'a'.
	if (self:match(inputtable, "MENU_ESCAPE") and inputtable.active) then
		if (self.key_kind == 'A' or self.key_kind == 'r') then
			self:renderline([[\#ff0000Cannot skip required keys/axes.\n\r\#ffffff]] .. self.label );
			return false;
		else
			return self:next_key();
		end
	end

-- analog inputs or just look at the 'press' part.
	if (self.key_kind == 'a' or self.key_kind == 'A') then
		if (inputtable.kind == "analog") then self:analoginput(inputtable); end
		
	elseif (inputtable.kind == "digital" and inputtable.active) then
		if (self:match(key) ~= nil) then
			print("Keyconf.lua, Notice: Button (" ..self:id(inputtable) .. ") already in use.\n");
		end

		self:set(inputtable);
		return self:next_key();
	end

	return false;
end

local function keyconf_labels(self)
	local labels = {};
	
	for key, value in pairs(self.table) do
		if string.sub(key, 1, 7) == "analog:" or
				string.sub(key, 1, 8) == "digital:" or
				string.sub(key, 1, 11) == "translated:" then
		else
			table.insert(labels, key);
		end
	end

	return labels;
end

local function keyconf_flush(self)
	zap_resource(self.keyfile);
	open_rawresource(self.keyfile);
	if (write_rawresource("local keyconf = {};\n") == false) then
		print("Couldn't save keysym.lua, check diskspace and permissions in theme folder.\n");
		close_rawresource();
		return;
    end

	for key, value in pairs(self.table) do
	if (type(value) == "table") then
	    write_rawresource( "keyconf[\"" .. key .. "\"] = {\"");
	    write_rawresource( table.concat(value, "\",\"") .. "\"};\n" );
	else
	    write_rawresource( "keyconf[\"" .. key .. "\"] = \"" .. value .. "\";\n" );
	end
    end

    write_rawresource("return keyconf;");
    close_rawresource();
end

local function keyconf_running(self)
	-- returns true if there is a configure session running
	if (self == nil) then
		return false;
	else
		return true;
	end
end

-- set the current working table.
-- for each stored entry, set prefix if defined 
function keyconf_create(nplayers, menugroup, playergroup, keyname)
	local restbl = {
		new = keyconf_new,
		match = keyconf_match,
		renderline = keyconf_renderline,
		next_key = keyconf_next_key,
		analoginput = keyconf_analog,
		defaultinput = keyconf_input,
		flush = keyconf_flush,
		id = keyconf_tbltoid,
		input = keyconf_input,
		destroy = keyconf_destroy,
		set = keyconf_set,
		labels = keyconf_labels,
		ignore_modifiers = false,
		n_players = nplayers,
		keyfile = keyname,
		input_playersel = keyconf_inp_playersel	
	};

	local players = 0;
	local buttons = {};

	if (keyname == nil) then restbl.keyfile = "keysym.lua"; end

-- command-line overrides
	for k,v in ipairs(arguments) do
		if (string.sub(v, 1, 8) == "keyname=") then
			local fn = string.sub(v, 9);
			if (string.len(fn) > 0) then
				restbl.keyfile = fn;
			end
		elseif (string.sub(v, 1, 10) == "nomodifier") then
			restbl.ignore_modifiers = true;
		elseif (v == "forcekeyconf") then
			zap_resource(restbl.keyfile);
		end
	end
	
	restbl.menu_group = menugroup and menugroup or default_menu_group;
	restbl.player_group = playergroup and playergroup or default_player_group;
	
	if ( resource(restbl.keyfile) ) then
		symfun = system_load(restbl.keyfile);
		restbl.table = symfun();
		restbl.active = true;
	else
		restbl:new();
		restbl.active = false;
	end

	return restbl;
end
