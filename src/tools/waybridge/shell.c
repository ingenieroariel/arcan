static bool shell_defer_handler(
	struct surface_request* req, struct arcan_shmif_cont* con)
{
	if (!req || !con){
		wl_resource_post_no_memory(req->target);
		return false;
	}

	struct wl_resource* ssurf = wl_resource_create(req->client->client,
		&wl_shell_surface_interface, wl_resource_get_version(req->target), req->id);

	if (!ssurf){
		wl_resource_post_no_memory(req->target);
		return false;
	}

	struct comp_surf* surf = wl_resource_get_user_data(req->target);
	wl_resource_set_implementation(ssurf, &ssurf_if, surf, NULL);
	surf->acon = *con;
	surf->cookie = 0xfeedface;
	return true;
}

static void shell_getsurf(struct wl_client* client,
	struct wl_resource* res, uint32_t id, struct wl_resource* surf_res)
{
	trace("get shell surface");
	struct comp_surf* surf = wl_resource_get_user_data(surf_res);
	request_surface(surf->client, &(struct surface_request){
		.segid = SEGID_APPLICATION,
		.target = surf_res,
		.id = id,
		.dispatch = shell_defer_handler,
		.client = surf->client,
		.source = surf
	});
}
